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
#>=Req: A | B
#>=Pkg: Y 1 1 x86_64
#>=Con: A | B
job install name X
result rules <inline>
#>rule job 3986285ed3e7fa05cb3367ca1e7f0d3d  X-1-1.x86_64@available
#>rule pkg 7d7fb2231997a6ba15d2c7c1d11d4ff1  A1-1-1.x86_64@available
#>rule pkg 7d7fb2231997a6ba15d2c7c1d11d4ff1  A2-1-1.x86_64@available
#>rule pkg 7d7fb2231997a6ba15d2c7c1d11d4ff1  B1-1-1.x86_64@available
#>rule pkg 7d7fb2231997a6ba15d2c7c1d11d4ff1  B2-1-1.x86_64@available
#>rule pkg 7d7fb2231997a6ba15d2c7c1d11d4ff1 -X-1-1.x86_64@available
nextjob
job install name Y
result rules <inline>
#>rule job 181d7955b2179c4ffdffedf0198f4807  Y-1-1.x86_64@available
#>rule pkg 0fd77d3093bfb9bc85606ae155bee7e0 -A1-1-1.x86_64@available
#>rule pkg 0fd77d3093bfb9bc85606ae155bee7e0 -Y-1-1.x86_64@available
#>rule pkg 45e18ba2f65a39574c1f4b1c77b565ec -A2-1-1.x86_64@available
#>rule pkg 45e18ba2f65a39574c1f4b1c77b565ec -Y-1-1.x86_64@available
#>rule pkg 69d83d7a676b6a65f79a6fc5bcf2ca6e -B1-1-1.x86_64@available
#>rule pkg 69d83d7a676b6a65f79a6fc5bcf2ca6e -Y-1-1.x86_64@available
#>rule pkg d6e57752cc50ab9fc0a218fda6dbf172 -B2-1-1.x86_64@available
#>rule pkg d6e57752cc50ab9fc0a218fda6dbf172 -Y-1-1.x86_64@available
