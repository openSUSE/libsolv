$: << "../../build/bindings/ruby"
require 'satsolver'
include SatSolver

pool = Pool.new
#puts pool.methods.sort

#s = pool.add_empty_repo

s = pool.create_repo('foo');
s.add_solv('../../testsuite/data.libzypp/basic-exercises/exercise-20-packages.solv');

installed = pool.create_repo('system');
installed.add_solv('../../testsuite/data.libzypp/basic-exercises/exercise-20-system.solv');

pool.each_repo do |repo|
  puts repo.name
end

s.each do |r|
  puts r
end

r = pool.find('G', s)
puts r

t = pool.create_transaction
t.install( r )

pool.prepare
pool.promoteepoch = 1

solv = Solver.new(pool, installed)

solv.fix_system = 0
solv.update_system = 0
solv.allow_downgrade = 0
solv.allow_uninstall = 0
solv.no_update_provide = 0

# solve the queue
solv.solve(t)

#solv.print_decisions

solv.each_to_install do |i|
  puts "to install #{i}"
end

solv.each_to_remove do |i|
  puts "to remove #{i}"
end
