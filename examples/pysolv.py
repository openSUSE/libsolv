#!/usr/bin/python

import sys
import os
import glob
import solv
import re
import tempfile
import time
import subprocess
import fnmatch
from stat import *
from solv import Pool, Repo, Dataiterator, Job
from iniparse import INIConfig
from optparse import OptionParser

def calc_checksum_stat(stat, type=solv.REPOKEY_TYPE_SHA256):
    chksum = solv.Chksum(type)
    chksum.add("1.1")
    chksum.add(str(stat[ST_DEV]))
    chksum.add(str(stat[ST_INO]))
    chksum.add(str(stat[ST_SIZE]))
    chksum.add(str(stat[ST_MTIME]))
    return chksum.raw()

def calc_checksum_fp(fp, type=solv.REPOKEY_TYPE_SHA256):
    chksum = solv.Chksum(type)
    chksum.addfp(fp)
    return chksum.raw()

def calccachepath(repo, repoext = None):
    path = re.sub(r'^\.', '_', repo['alias'])
    if repoext:
	path += "_" + repoext + ".solvx"
    else:
	path += ".solv"
    return "/var/cache/solv/" + re.sub(r'[/]', '_', path)
    
def usecachedrepo(repo, repoext, cookie, mark=False):
    handle = repo['handle']
    try: 
        repopath = calccachepath(repo, repoext)
        f = open(repopath, 'r')
	f.seek(-32, os.SEEK_END)
	fcookie = f.read(32)
	if len(fcookie) != 32:
	    return False
	if cookie and fcookie != cookie:
	    return False
	if repo['alias'] != '@System' and not repoext:
	    f.seek(-32 * 2, os.SEEK_END)
	    fextcookie = f.read(32)
	    if len(fextcookie) != 32:
		return False
        f.seek(0)
        flags = 0
        if repoext:
            flags = Repo.REPO_USE_LOADING|Repo.REPO_EXTEND_SOLVABLES
            if repoext != 'DL':
		flags |= Repo.REPO_LOCALPOOL
        if not repo['handle'].add_solv(f, flags):
	    return False
	if repo['alias'] != '@System' and not repoext:
	    repo['cookie'] = fcookie
	    repo['extcookie'] = fextcookie
	if mark:
	    # no futimes in python?
	    try:
		os.utime(repopath, None)
	    except e:
		pass
    except IOError, e:
	return False
    return True

def writecachedrepo(repo, repoext, cookie, info=None):
    try:
	if not os.path.isdir("/var/cache/solv"):
	    os.mkdir("/var/cache/solv", 0755);
	(fd, tmpname) = tempfile.mkstemp(prefix='.newsolv-', dir='/var/cache/solv')
	os.fchmod(fd, 0444)
        f = os.fdopen(fd, 'w+')
	if not info:
	    repo['handle'].write(f)
	elif repoext:
	    info.write(f)
	else:
	    repo['handle'].write_first_repodata(f)
	if repo['alias'] != '@System' and not repoext:
	    if 'extcookie' not in repo:
		# create unique id
		extcookie = calc_checksum_stat(os.fstat(f.fileno()))
		extcookie = ''.join(chr(ord(s)^ord(c)) for s,c in zip(extcookie, cookie))
		if ord(extcookie[0]) == 0:
		    extcookie[0] = chr(1)
		repo['extcookie'] = extcookie
	    f.write(repo['extcookie'])
	f.write(cookie)
	f.close()
	os.rename(tmpname, calccachepath(repo, repoext))
    except IOError, e:
	if tmpname:
	    os.unlink(tmpname)

def curlfopen(repo, file, uncompress, chksum, chksumtype, badchecksum=None):
    baseurl = repo['baseurl']
    url = re.sub(r'/$', '', baseurl) + '/' + file;
    f = tempfile.TemporaryFile()
    st = subprocess.call(['curl', '-f', '-s', '-L', url], stdout=f.fileno())
    if os.lseek(f.fileno(), 0, os.SEEK_CUR) == 0 and (st == 0 or not chksumtype):
	return None
    os.lseek(f.fileno(), 0, os.SEEK_SET)
    if st:
	print "%s: download error %d" % (file, st)
	if badchecksum:
	    badchecksum['True'] = 'True'
        return None
    if chksumtype:
	fchksum = solv.Chksum(chksumtype)
	if not fchksum:
	    print "%s: unknown checksum type" % file
	    if badchecksum:
		badchecksum['True'] = 'True'
	    return None
	fchksum.addfd(f.fileno())
	if not fchksum.matches(chksum):
	    print "%s: checksum mismatch" % file
	    if badchecksum:
		badchecksum['True'] = 'True'
	    return None
    if uncompress:
	return solv.xfopen_fd(file, os.dup(f.fileno()))
    return solv.xfopen_fd("", os.dup(f.fileno()))

def repomd_find(repo, what):
    di = repo['handle'].dataiterator_new(solv.SOLVID_META, solv.REPOSITORY_REPOMD_TYPE, what, Dataiterator.SEARCH_STRING)
    di.prepend_keyname(solv.REPOSITORY_REPOMD);
    for d in di:
        d.setpos_parent()
        filename = d.pool.lookup_str(solv.SOLVID_POS, solv.REPOSITORY_REPOMD_LOCATION);
        chksum, chksumtype = d.pool.lookup_bin_checksum(solv.SOLVID_POS, solv.REPOSITORY_REPOMD_CHECKSUM);
        if filename and not chksumtype:
	    print "no %s file checksum!" % filename
	    filename = None
	    chksum = None
        if filename:
            return (filename, chksum, chksumtype)
    return (None, None, None)

def validarch(pool, arch):
    if not arch:
	return False
    id = pool.str2id(arch, False)
    if not id:
	return False
    return pool.isknownarch(id)

def limitjobs(pool, jobs, flags, evr):
    njobs = []
    for j in jobs:
	how = j.how;
	sel = how & Job.SOLVER_SELECTMASK
	what = pool.rel2id(j.what, evr, flags)
        if flags == solv.REL_ARCH:
	    how |= Job.SOLVER_SETARCH
	if flags == solv.REL_EQ and sel == Job.SOLVER_SOLVABLE_NAME:
	    if pool.id2str(evr).find('-') >= 0:
		how |= Job.SOLVER_SETEVR
	    else:
		how |= Job.SOLVER_SETEV
	njobs.append(Job(how, what))
    return njobs

def limitjobs_arch(pool, jobs, flags, evr):
    m = re.match(r'(.+)\.(.+?)$', evr)
    if m and validarch(pool, m.group(2)):
	jobs = limitjobs(pool, jobs, solv.REL_ARCH, pool.str2id(m.group(2)))
	return limitjobs(pool, jobs, flags, pool.str2id(m.group(1)))
    else:
	return limitjobs(pool, jobs, flags, pool.str2id(evr))

def mkjobs(pool, cmd, arg):
    if len(arg) and arg[0] == '/':
        if re.search(r'[[*?]', arg):
	    type = Dataiterator.SEARCH_GLOB
	else:
	    type = Dataiterator.SEARCH_STRING
        if cmd == 'rm' or cmd == 'erase':
	    di = pool.installed.dataiterator_new(0, solv.SOLVABLE_FILELIST, arg, type | Dataiterator.SEARCH_FILES|Dataiterator.SEARCH_COMPLETE_FILELIST)
	else:
	    di = pool.dataiterator_new(0, solv.SOLVABLE_FILELIST, arg, type | Dataiterator.SEARCH_FILES|Dataiterator.SEARCH_COMPLETE_FILELIST)
        matches = []
	for d in di:
	    s = d.solvable
	    if s and s.installable():
		matches.append(s.id)
		di.skip_solvable()	# one match is enough
	if len(matches):
	    print "[using file list match for '%s']" % arg
	    if len(matches) > 1:
		return [ Job(Job.SOLVER_SOLVABLE_ONE_OF, pool.towhatprovides(matches)) ]
	    else:
		return [ Job(Job.SOLVER_SOLVABLE | Job.SOLVER_NOAUTOSET, matches[0]) ]
    m = re.match(r'(.+?)\s*([<=>]+)\s*(.+?)$', arg)
    if m:
	(name, rel, evr) = m.group(1, 2, 3);
	flags = 0
	if rel.find('<') >= 0: flags |= solv.REL_LT
	if rel.find('=') >= 0: flags |= solv.REL_EQ 
	if rel.find('>') >= 0: flags |= solv.REL_GT
	jobs = depglob(pool, name, True, True)
	if len(jobs):
	    return limitjobs(pool, jobs, flags, pool.str2id(evr))
	m = re.match(r'(.+)\.(.+?)$', name)
	if m and validarch(pool, m.group(2)):
	    jobs = depglob(pool, m.group(1), True, True)
	    if len(jobs):
		jobs = limitjobs(pool, jobs, solv.REL_ARCH, pool.str2id(m.group(2)))
		return limitjobs(pool, jobs, flags, pool.str2id(evr))
    else:
	jobs = depglob(pool, arg, True, True)
        if len(jobs):
	    return jobs
	m = re.match(r'(.+)\.(.+?)$', arg)
	if m and validarch(pool, m.group(2)):
	    jobs = depglob(pool, m.group(1), True, True)
	    if len(jobs):
		return limitjobs(pool, jobs, solv.REL_ARCH, pool.str2id(m.group(2)))
	m = re.match(r'(.+)-(.+?)$', arg)
	if m:
	    jobs = depglob(pool, m.group(1), True, False)
	    if len(jobs):
		return limitjobs_arch(pool, jobs, solv.REL_EQ, m.group(2))
	m = re.match(r'(.+)-(.+?-.+?)$', arg)
	if m:
	    jobs = depglob(pool, m.group(1), True, False)
	    if len(jobs):
		return limitjobs_arch(pool, jobs, solv.REL_EQ, m.group(2))
    return []
    
	    
def depglob(pool, name, globname, globdep):
    id = pool.str2id(name, False)
    if id:
	match = False
	for s in pool.providers(id):
	    if globname and s.nameid == id:
		return [ Job(Job.SOLVER_SOLVABLE_NAME, id) ]
	    match = True
	if match:
	    if globname and globdep:
		print "[using capability match for '%s']" % name
	    return [ Job(Job.SOLVER_SOLVABLE_PROVIDES, id) ]
    if not re.search(r'[[*?]', name):
	return []
    if globname:
	# try name glob
	idmatches = {}
	for s in pool.solvables:
	    if s.installable() and fnmatch.fnmatch(s.name, name):
		idmatches[s.nameid] = True
	if len(idmatches):
	    return [ Job(Job.SOLVER_SOLVABLE_NAME, id) for id in sorted(idmatches.keys()) ]
    if globdep:
	# try dependency glob
	idmatches = {}
	for id in pool.allprovidingids():
	    if fnmatch.fnmatch(pool.id2str(id), name):
		idmatches[id] = True
	if len(idmatches):
	    print "[using capability match for '%s']" % name
	    return [ Job(Job.SOLVER_SOLVABLE_PROVIDES, id) for id in sorted(idmatches.keys()) ]
    return []
    

parser = OptionParser(usage="usage: solv.py [options] COMMAND")
(options, args) = parser.parse_args()
if not args:
    parser.print_help(sys.stderr)
    sys.exit(1)

cmd = args[0]
args = args[1:]

pool = solv.Pool()
pool.setarch(os.uname()[4])

repos = []
for reposdir in ["/etc/zypp/repos.d"]:
    if not os.path.isdir(reposdir):
	continue
    for reponame in sorted(glob.glob('%s/*.repo' % reposdir)):
	cfg = INIConfig(open(reponame))
	for alias in cfg:
	    repo = cfg[alias]
	    repo['alias'] = alias
	    if 'priority' not in repo:
		repo['priority'] = 99
	    if 'autorefresh' not in repo:
		repo['autorefresh'] = 1
	    repo['metadata_expire'] = 900
	    repos.append(repo)

print "rpm database:",
sysrepo = { 'alias': '@System' }
sysrepo['handle'] = pool.add_repo(sysrepo['alias'])
sysrepo['handle'].appdata = sysrepo
pool.installed = sysrepo['handle']
sysrepostat = os.stat("/var/lib/rpm/Packages")
sysrepocookie = calc_checksum_stat(sysrepostat)
if usecachedrepo(sysrepo, None, sysrepocookie):
    print "cached"
else:
    print "reading"
    sysrepo['handle'].add_products("/etc/products.d", Repo.REPO_NO_INTERNALIZE);
    sysrepo['handle'].add_rpmdb(None)
    writecachedrepo(sysrepo, None, sysrepocookie)

for repo in repos:
    if not int(repo.enabled):
	continue
    repo['handle'] = pool.add_repo(repo['alias'])
    repo['handle'].appdata = repo
    repo['handle'].priority = 99 - repo['priority']
    if repo['autorefresh']:
	dorefresh = True
    if dorefresh:
	try:
	    st = os.stat(calccachepath(repo))
	    if time.time() - st[ST_MTIME] < repo['metadata_expire']:
		dorefresh = False
	except OSError, e:
	    pass
    if not dorefresh and usecachedrepo(repo, None, None):
	print "repo: '%s': cached" % repo['alias']
	continue

    badchecksum = {}

    print "rpmmd repo '%s':" % repo['alias'],
    sys.stdout.flush()
    f = curlfopen(repo, "repodata/repomd.xml", False, None, None)
    if not f:
	print "no repomd.xml file, skipped"
	repo['handle'].free(True)
	del repo['handle']
	continue
    repo['cookie'] = calc_checksum_fp(f)
    if usecachedrepo(repo, None, repo['cookie'], True):
	print "cached"
        solv.xfclose(f)
	continue
    repo['handle'].add_repomdxml(f, 0)
    solv.xfclose(f)
    print "fetching"
    (filename, filechksum, filechksumtype) = repomd_find(repo, 'primary')
    if filename:
	f = curlfopen(repo, filename, True, filechksum, filechksumtype, badchecksum)
	if f:
	    repo['handle'].add_rpmmd(f, None, 0)
	    solv.xfclose(f)
	if badchecksum:
	    continue	# hopeless, need good primary
    (filename, filechksum, filechksumtype) = repomd_find(repo, 'updateinfo')
    if filename:
	f = curlfopen(repo, filename, True, filechksum, filechksumtype, badchecksum)
	if f:
	    repo['handle'].add_updateinfoxml(f, 0)
	    solv.xfclose(f)
    # if the checksum was bad we work with the data we got, but don't cache it
    if 'True' not in badchecksum:
	writecachedrepo(repo, None, repo['cookie'])
    
if cmd == 'se' or cmd == 'search':
    matches = {}
    print "searching for", args[1]
    di = pool.dataiterator_new(0, solv.SOLVABLE_NAME, args[1], Dataiterator.SEARCH_SUBSTRING|Dataiterator.SEARCH_NOCASE)
    for d in di:
	matches[di.solvid] = True
    for solvid in sorted(matches.keys()):
	print " - %s: %s" % (pool.solvid2str(solvid), pool.lookup_str(solvid, solv.SOLVABLE_SUMMARY))
    exit(0)

# XXX: insert rewrite_repos function

pool.addfileprovides()
pool.createwhatprovides()

jobs = []
for arg in args:
    argjob = mkjobs(pool, cmd, arg)
    jobs += argjob

if cmd == 'li' or cmd == 'list' or cmd == 'info':
    if not jobs:
	print "no package matched."
	exit(1)
    for job in jobs:
	for s in pool.jobsolvables(job):
	    if cmd == 'info':
		print "Name:        %s" % s.str()
		print "Repo:        %s" % s.repo.name
		print "Summary:     %s" % s.lookup_str(solv.SOLVABLE_SUMMARY)
		str = s.lookup_str(solv.SOLVABLE_URL)
		if str:
		    print "Url:         %s" % str
		str = s.lookup_str(solv.SOLVABLE_LICENSE)
		if str:
		    print "License:     %s" % str
		print "Description:\n%s" % s.lookup_str(solv.SOLVABLE_DESCRIPTION)
		print
	    else:
		print "  - %s [%s]" % (s.str(), s.repo.name)
		print "    %s" % s.lookup_str(solv.SOLVABLE_SUMMARY)
    exit(0)

if cmd == 'in' or cmd == 'install' or cmd == 'rm' or cmd == 'erase' or cmd == 'up':
    if cmd == 'up' and not jobs:
	jobs = [ Job(Job.SOLVER_SOLVABLE_ALL, 0) ]
    if not jobs:
	print "no package matched."
	exit(1)
    for job in jobs:
	if cmd == 'up':
	    if job.how == Job.SOLVER_SOLVABLE_ALL or filter(lambda s: s.isinstalled(), pool.jobsolvables(job)):
		job.how |= Job.SOLVER_UPDATE
	    else:
		job.how |= Job.SOLVER_INSTALL
	if cmd == 'in' or cmd == 'install':
	    job.how |= Job.SOLVER_INSTALL
	elif cmd == 'rm' or cmd == 'erase':
	    job.how |= Job.SOLVER_ERASE

    #pool.set_debuglevel(2)
    solver = pool.create_solver()
    problems = solver.solve(jobs)
    if problems:
	for problem in problems:
	    print "Problem %d:" % problem.id
	    r = problem.findproblemrule()
	    type, source, target, dep = solver.ruleinfo(r)
	    print type, source, target, dep
    
# vim: sw=4 et
