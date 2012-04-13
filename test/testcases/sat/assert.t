repo system 0 testtags assert-system.repo
system x86_64 rpm system
job erase provides AA [weak]
job install pkg B-1-1.x86_64@system
result transaction,problems <inline>
