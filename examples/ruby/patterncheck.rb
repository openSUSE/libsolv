#
# patterncheck.rb
#
# First approach to 'rpm-only' pattern management
# - seperate pattern into 'pattern deps' and 'package deps'
# - solve patterns only
# - use the resulting packages deps to check the system
#

#
# Fully print dependency
#

def fullprint_deps name, deps
  print "#{name}: "
  first = true
  deps.each do |d|
    print "\t" unless first
    puts "#{d}"
    first = false
  end
  puts if first
end

#
# Fully print solvable s, including dependencies
#

def fullprint_solvable solvable
  puts "Solvable #{solvable}"
  fullprint_deps "Provides", solvable.provides
  fullprint_deps "Requires", solvable.requires
  fullprint_deps "Conflicts", solvable.conflicts
  fullprint_deps "Obsoletes", solvable.obsoletes
  fullprint_deps "Recommends", solvable.recommends
  fullprint_deps "Suggests", solvable.suggests
  fullprint_deps "Supplements", solvable.supplements
  fullprint_deps "Enhances", solvable.enhances
  fullprint_deps "Freshens", solvable.freshens
end


#
# strip 'pattern:' prefix from name
#

def strip_prefix name
  return name[8..-1] if name =~ /^pattern:/
  name
end

# split_dep( deps, pdeps )
# add pattern: deps to pdeps
# return non-pattern relations stripped of pool
#  (as array of [ name, op, evr ])
#

def split_dep deps, pdeps
#  puts "split_dep #{deps} [#{deps.size}], #{pdeps} [#{pdeps.size}]"
  relations = Array.new
  deps.each { |d|
    name = strip_prefix d.name
    if d.name =~ /^pattern:/
#      puts "Relation.new( #{name}, #{d.op}, #{d.evr})"
      pdeps << Relation.new( pdeps.solvable.repo.pool, name, d.op, d.evr )
    else
      relations << [ name, d.op, d.evr ]
    end
  }
  relations
end

# split_deps()
# split dependencies of (pattern) p
# into (pattern) new_pat for pattern: deps
# and my_deps for non-pattern: deps
#   my_deps => Hash { string(name) => Hash { symbol(dependency) => Array [ name, op, evr ] } }
#

def split_deps p, new_pat
#  puts "split_deps pattern"
#  fullprint_solvable p
#  puts "new_pat"
#  fullprint_solvable new_pat
#  puts
  my_deps = Hash.new
  my_deps[:provides] = split_dep p.provides, new_pat.provides
  my_deps[:requires] = split_dep p.requires, new_pat.requires
  my_deps[:conflicts] = split_dep p.conflicts, new_pat.conflicts
  my_deps[:obsoletes] = split_dep p.obsoletes, new_pat.obsoletes
  my_deps[:recommends] = split_dep p.recommends, new_pat.recommends
  my_deps[:suggests] = split_dep p.suggests, new_pat.suggests
  my_deps[:supplements] = split_dep p.supplements, new_pat.supplements
  my_deps[:enhances] = split_dep p.enhances, new_pat.enhances
  my_deps[:freshens] = split_dep p.freshens, new_pat.freshens
#  puts "==> new_pat"
#  fullprint_solvable new_pat
  my_deps
end


#
# check status of solved.decision against system(Pool) and my_deps (Hash of package deps)
#
def check_status system, my_deps, solved
  solver = system.create_solver
  t = system.create_transaction
  # collect dependencies
  solved.each_decision do |d|
    deps = my_deps[d.solvable.name]
    case d.op
    when DEC_INSTALL
      deps[:requires].each do |r|
	t.install system.create_relation( r[0], r[1], r[2] )
      end
      deps[:obsoletes].each do |r|
	t.remove system.create_relation( r[0], r[1], r[2] )
      end
      deps[:conflicts].each do |r|
	t.remove system.create_relation( r[0], r[1], r[2] )
      end
    when DEC_REMOVE
    else
      raise "******Unhandled #{d.op}"
    end
  end
  solver.solve t unless t.empty?

  if solver.problems?
    return false
    solver.each_problem t do |p|
      puts "\t #{p}"
    end
  else
    return true
  end
  if nil
    t.each do |a|
      print "\t"
      puts (
	    case a.cmd
	    when SatSolver::INSTALL_SOLVABLE: "install #{a.solvable}"
	    when SatSolver::REMOVE_SOLVABLE: "remove #{a.solvable}"
	    when SatSolver::INSTALL_SOLVABLE_NAME: "install by name #{a.name}"
	    when SatSolver::REMOVE_SOLVABLE_NAME: "remove by name #{a.name}"
	    when SatSolver::INSTALL_SOLVABLE_PROVIDES: "install by relation #{a.relation}"
	    when SatSolver::REMOVE_SOLVABLE_PROVIDES: "remove by relation #{a.relation}"
	    else "<NONE>"
	    end
	    )
    end
  end
end

$: << "../../build/bindings/ruby"
require 'satsolver'
include SatSolver
require 'problem'
require 'decision'

pool = Pool.new( "x86_64" )

patterns = pool.create_repo 'patterns'
patterns.add_solv '10_3-x86_64-patterns.solv'

puts "Found #{patterns.size} patterns"

#
# Now split-off package deps from the patterns and
# keep them separate.
#

my_pool = Pool.new
my_pool.arch = "x86_64"

my_pats = my_pool.create_repo 'patterns'
my_deps = Hash.new

i = 0
patterns.each do |p|
  i += 1
#  fullprint_solvable p
  name = strip_prefix p.name
  new_pat = my_pats.create_solvable name, p.evr, p.arch
  my_deps[name] = split_deps p, new_pat
#  break if i > 1
end

my_pool.promoteepoch = 1

puts "My pool has #{my_pool.count_repos} repos and #{my_pool.size} solvables"

system = Pool.new "x86_64"
system.add_rpmdb "/"

i = 0
my_pats.each do |p|
  i += 1
  solver = my_pool.create_solver

  solver.fix_system = 0
  solver.update_system = 0
  solver.allow_downgrade = 0
  solver.allow_uninstall = 0
  solver.no_update_provide = 0
  
  t = my_pool.create_transaction
  t.install p
  solver.solve t
#  fullprint_solvable p
#  print "Install #{p} "
  if solver.problems?
    puts "*** Failed"
    solver.each_problem t do |p|
#      puts p
    end
  else
#    puts "Succeeded"
    result = check_status system, my_deps, solver
    if result
      puts "Yes: #{p}"
    else
      puts "No:  #{p}"
    end
    solver.each_decision do |d|
#      puts d
    end
  end
#  break if i > 1
end
