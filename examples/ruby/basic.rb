require 'satsolver'

include SatSolver

pool = Pool.new
#puts pool.methods.sort

#s = pool.add_empty_repo

f = File.open('../../testsuite/data.libzypp/basic-exercises/exercise-20-packages.solv', 'r')
s = pool.create_repo('foo');
s.add_solv(f);

f = File.open('../../testsuite/data.libzypp/basic-exercises/exercise-20-system.solv', 'r')
installed = pool.create_repo('system');
installed.add_solv(f);

pool.each_repo do |repo|
  puts repo.name
end

s.each_solvable do |r|
  puts r
end

q = Queue.new
puts q.empty?

r = pool.select_solvable(s, 'G')
puts r

# push one command and one resolvable to the queue
q.push(SOLVER_INSTALL_SOLVABLE)
q.push(r)

pool.prepare
pool.promoteepoch = 1

# no packages installed so use add_empty_repo
solv = Solver.new(pool, installed)

solv.fixsystem = 0
solv.updatesystem = 0
solv.allowdowngrade = 0
solv.allowuninstall = 0
solv.noupdateprovide = 0

# solve the queue
solv.solve(q)

#solv.print_decisions

solv.each_to_install do |i|
  puts "to install #{i}"
end

solv.each_to_remove do |i|
  puts "to remove #{i}"
end
