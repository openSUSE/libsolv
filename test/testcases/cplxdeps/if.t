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
#>=Req: A <IF> B
#>=Pkg: Y 1 1 x86_64
#>=Con: A <IF> B
job install name X
result rules <inline>
#>rule job 3986285ed3e7fa05cb3367ca1e7f0d3d  X-1-1.x86_64@available
#>rule pkg 2c561cfd067b5ec806fd5c4fcd2ac761  A1-1-1.x86_64@available
#>rule pkg 2c561cfd067b5ec806fd5c4fcd2ac761  A2-1-1.x86_64@available
#>rule pkg 2c561cfd067b5ec806fd5c4fcd2ac761 -B1-1-1.x86_64@available
#>rule pkg 2c561cfd067b5ec806fd5c4fcd2ac761 -X-1-1.x86_64@available
#>rule pkg 72c9298b01fec76aae2722e2cdfdc776  A1-1-1.x86_64@available
#>rule pkg 72c9298b01fec76aae2722e2cdfdc776  A2-1-1.x86_64@available
#>rule pkg 72c9298b01fec76aae2722e2cdfdc776 -B2-1-1.x86_64@available
#>rule pkg 72c9298b01fec76aae2722e2cdfdc776 -X-1-1.x86_64@available
nextjob
job install name Y
result rules <inline>
#>rule job 181d7955b2179c4ffdffedf0198f4807  Y-1-1.x86_64@available
#>rule pkg 0fd77d3093bfb9bc85606ae155bee7e0 -A1-1-1.x86_64@available
#>rule pkg 0fd77d3093bfb9bc85606ae155bee7e0 -Y-1-1.x86_64@available
#>rule pkg 45e18ba2f65a39574c1f4b1c77b565ec -A2-1-1.x86_64@available
#>rule pkg 45e18ba2f65a39574c1f4b1c77b565ec -Y-1-1.x86_64@available
#>rule pkg 88a787532b7e170c57d67c5123982b0d  B1-1-1.x86_64@available
#>rule pkg 88a787532b7e170c57d67c5123982b0d  B2-1-1.x86_64@available
#>rule pkg 88a787532b7e170c57d67c5123982b0d -Y-1-1.x86_64@available
