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
#>=Pkg: C1 1 1 x86_64
#>=Prv: C
#>=Pkg: C2 1 1 x86_64
#>=Prv: C
#>=Pkg: X 1 1 x86_64
#>=Req: A <IF> (B <ELSE> C)
#>=Pkg: Y 1 1 x86_64
#>=Con: A <IF> (B <ELSE> C)
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
#>rule pkg dee60af94c8ea49c9f99362dcb65806a  B1-1-1.x86_64@available
#>rule pkg dee60af94c8ea49c9f99362dcb65806a  B2-1-1.x86_64@available
#>rule pkg dee60af94c8ea49c9f99362dcb65806a  C1-1-1.x86_64@available
#>rule pkg dee60af94c8ea49c9f99362dcb65806a  C2-1-1.x86_64@available
#>rule pkg dee60af94c8ea49c9f99362dcb65806a -X-1-1.x86_64@available
nextjob
job install name Y
result rules <inline>
#>rule job 181d7955b2179c4ffdffedf0198f4807  Y-1-1.x86_64@available
#>rule pkg 05a449a52d4792a0b847f482f5d7b509 -A1-1-1.x86_64@available
#>rule pkg 05a449a52d4792a0b847f482f5d7b509 -B2-1-1.x86_64@available
#>rule pkg 05a449a52d4792a0b847f482f5d7b509 -Y-1-1.x86_64@available
#>rule pkg 2823a39a3d1ae2f7d45ca97e97f5bf8e -A1-1-1.x86_64@available
#>rule pkg 2823a39a3d1ae2f7d45ca97e97f5bf8e -C1-1-1.x86_64@available
#>rule pkg 2823a39a3d1ae2f7d45ca97e97f5bf8e -Y-1-1.x86_64@available
#>rule pkg 46a6ab5f0c299f23867d74570cc179b8  B1-1-1.x86_64@available
#>rule pkg 46a6ab5f0c299f23867d74570cc179b8  B2-1-1.x86_64@available
#>rule pkg 46a6ab5f0c299f23867d74570cc179b8 -C1-1-1.x86_64@available
#>rule pkg 46a6ab5f0c299f23867d74570cc179b8 -Y-1-1.x86_64@available
#>rule pkg 85eac3dcd30bb5f51ad50b6644455822 -A2-1-1.x86_64@available
#>rule pkg 85eac3dcd30bb5f51ad50b6644455822 -B2-1-1.x86_64@available
#>rule pkg 85eac3dcd30bb5f51ad50b6644455822 -Y-1-1.x86_64@available
#>rule pkg 8611a3d851adbd063a5d34a3ad4804e6 -A1-1-1.x86_64@available
#>rule pkg 8611a3d851adbd063a5d34a3ad4804e6 -B1-1-1.x86_64@available
#>rule pkg 8611a3d851adbd063a5d34a3ad4804e6 -Y-1-1.x86_64@available
#>rule pkg 92d66d3ad45eaa7d443a05245c9a0eaf -A2-1-1.x86_64@available
#>rule pkg 92d66d3ad45eaa7d443a05245c9a0eaf -C1-1-1.x86_64@available
#>rule pkg 92d66d3ad45eaa7d443a05245c9a0eaf -Y-1-1.x86_64@available
#>rule pkg ba6ba0db8d7421f83f7df6cc4df6df7d -A2-1-1.x86_64@available
#>rule pkg ba6ba0db8d7421f83f7df6cc4df6df7d -B1-1-1.x86_64@available
#>rule pkg ba6ba0db8d7421f83f7df6cc4df6df7d -Y-1-1.x86_64@available
#>rule pkg c3f6b37277eb48011d0e68c11323ea3e  B1-1-1.x86_64@available
#>rule pkg c3f6b37277eb48011d0e68c11323ea3e  B2-1-1.x86_64@available
#>rule pkg c3f6b37277eb48011d0e68c11323ea3e -C2-1-1.x86_64@available
#>rule pkg c3f6b37277eb48011d0e68c11323ea3e -Y-1-1.x86_64@available
#>rule pkg c450fe9f8321047ee2cd1e6fc9f34df6 -A2-1-1.x86_64@available
#>rule pkg c450fe9f8321047ee2cd1e6fc9f34df6 -C2-1-1.x86_64@available
#>rule pkg c450fe9f8321047ee2cd1e6fc9f34df6 -Y-1-1.x86_64@available
#>rule pkg da9eb4921698e87f788a89bd25cf75b8 -A1-1-1.x86_64@available
#>rule pkg da9eb4921698e87f788a89bd25cf75b8 -C2-1-1.x86_64@available
#>rule pkg da9eb4921698e87f788a89bd25cf75b8 -Y-1-1.x86_64@available
