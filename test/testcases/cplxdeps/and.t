feature complex_deps
repo available 0 testtags <inline>
#>=Pkg: A1 1 1 x86_64
#>=Prv: A
#>=Pkg: A2 1 1 x86_64
#>=Prv: A
#>=Pkg: B1 1 1 x86_64
#>=Prv: B
#>=Pkg: B2 1 1 x86_64
#>=Prv: B
#>=Pkg: X 1 1 x86_64
#>=Req: A & B
#>=Pkg: Y 1 1 x86_64
#>=Con: A & B
job install name X
result rules <inline>
#>rule job 3986285ed3e7fa05cb3367ca1e7f0d3d  X-1-1.x86_64@available
#>rule pkg 8d94817282778a96505d2865a9b6c417  B1-1-1.x86_64@available
#>rule pkg 8d94817282778a96505d2865a9b6c417  B2-1-1.x86_64@available
#>rule pkg 8d94817282778a96505d2865a9b6c417 -X-1-1.x86_64@available
#>rule pkg 9294f4d070a449a787a945d757c853ac  A1-1-1.x86_64@available
#>rule pkg 9294f4d070a449a787a945d757c853ac  A2-1-1.x86_64@available
#>rule pkg 9294f4d070a449a787a945d757c853ac -X-1-1.x86_64@available
nextjob
job install name Y
result rules <inline>
#>rule job 181d7955b2179c4ffdffedf0198f4807  Y-1-1.x86_64@available
#>rule pkg 05a449a52d4792a0b847f482f5d7b509 -A1-1-1.x86_64@available
#>rule pkg 05a449a52d4792a0b847f482f5d7b509 -B2-1-1.x86_64@available
#>rule pkg 05a449a52d4792a0b847f482f5d7b509 -Y-1-1.x86_64@available
#>rule pkg 85eac3dcd30bb5f51ad50b6644455822 -A2-1-1.x86_64@available
#>rule pkg 85eac3dcd30bb5f51ad50b6644455822 -B2-1-1.x86_64@available
#>rule pkg 85eac3dcd30bb5f51ad50b6644455822 -Y-1-1.x86_64@available
#>rule pkg 8611a3d851adbd063a5d34a3ad4804e6 -A1-1-1.x86_64@available
#>rule pkg 8611a3d851adbd063a5d34a3ad4804e6 -B1-1-1.x86_64@available
#>rule pkg 8611a3d851adbd063a5d34a3ad4804e6 -Y-1-1.x86_64@available
#>rule pkg ba6ba0db8d7421f83f7df6cc4df6df7d -A2-1-1.x86_64@available
#>rule pkg ba6ba0db8d7421f83f7df6cc4df6df7d -B1-1-1.x86_64@available
#>rule pkg ba6ba0db8d7421f83f7df6cc4df6df7d -Y-1-1.x86_64@available
