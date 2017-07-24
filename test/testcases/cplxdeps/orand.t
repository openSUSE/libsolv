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
#>=Pkg: D1 1 1 x86_64
#>=Prv: D
#>=Pkg: D2 1 1 x86_64
#>=Prv: D
#>=Pkg: X 1 1 x86_64
#>=Req: (A | B) & (C | D)
#>=Pkg: Y 1 1 x86_64
#>=Con: (A | B) & (C | D)
job install name X
result rules <inline>
#>rule job 3986285ed3e7fa05cb3367ca1e7f0d3d  X-1-1.x86_64@available
#>rule pkg 7d7fb2231997a6ba15d2c7c1d11d4ff1  A1-1-1.x86_64@available
#>rule pkg 7d7fb2231997a6ba15d2c7c1d11d4ff1  A2-1-1.x86_64@available
#>rule pkg 7d7fb2231997a6ba15d2c7c1d11d4ff1  B1-1-1.x86_64@available
#>rule pkg 7d7fb2231997a6ba15d2c7c1d11d4ff1  B2-1-1.x86_64@available
#>rule pkg 7d7fb2231997a6ba15d2c7c1d11d4ff1 -X-1-1.x86_64@available
#>rule pkg cb5c72eeb44f643493f3df3e7e62f9dc  C1-1-1.x86_64@available
#>rule pkg cb5c72eeb44f643493f3df3e7e62f9dc  C2-1-1.x86_64@available
#>rule pkg cb5c72eeb44f643493f3df3e7e62f9dc  D1-1-1.x86_64@available
#>rule pkg cb5c72eeb44f643493f3df3e7e62f9dc  D2-1-1.x86_64@available
#>rule pkg cb5c72eeb44f643493f3df3e7e62f9dc -X-1-1.x86_64@available
nextjob
job install name Y
result rules <inline>
#>rule job 181d7955b2179c4ffdffedf0198f4807  Y-1-1.x86_64@available
#>rule pkg 16cbba917580ca34f91961a74cfd07c2 -B1-1-1.x86_64@available
#>rule pkg 16cbba917580ca34f91961a74cfd07c2 -D1-1-1.x86_64@available
#>rule pkg 16cbba917580ca34f91961a74cfd07c2 -Y-1-1.x86_64@available
#>rule pkg 1db9a5695f062c299dbb815adf4f2374 -A1-1-1.x86_64@available
#>rule pkg 1db9a5695f062c299dbb815adf4f2374 -D2-1-1.x86_64@available
#>rule pkg 1db9a5695f062c299dbb815adf4f2374 -Y-1-1.x86_64@available
#>rule pkg 22f3f4d055f6daaeb6f5622907a9206f -A2-1-1.x86_64@available
#>rule pkg 22f3f4d055f6daaeb6f5622907a9206f -D1-1-1.x86_64@available
#>rule pkg 22f3f4d055f6daaeb6f5622907a9206f -Y-1-1.x86_64@available
#>rule pkg 2823a39a3d1ae2f7d45ca97e97f5bf8e -A1-1-1.x86_64@available
#>rule pkg 2823a39a3d1ae2f7d45ca97e97f5bf8e -C1-1-1.x86_64@available
#>rule pkg 2823a39a3d1ae2f7d45ca97e97f5bf8e -Y-1-1.x86_64@available
#>rule pkg 33a12cadfd9dd12dc60a3e250ae9f92a -B2-1-1.x86_64@available
#>rule pkg 33a12cadfd9dd12dc60a3e250ae9f92a -C2-1-1.x86_64@available
#>rule pkg 33a12cadfd9dd12dc60a3e250ae9f92a -Y-1-1.x86_64@available
#>rule pkg 48a9e473b42b382f7cf46ac33e98ba76 -B2-1-1.x86_64@available
#>rule pkg 48a9e473b42b382f7cf46ac33e98ba76 -C1-1-1.x86_64@available
#>rule pkg 48a9e473b42b382f7cf46ac33e98ba76 -Y-1-1.x86_64@available
#>rule pkg 714e9978244eb65bc57b7c4fac5a0430 -B1-1-1.x86_64@available
#>rule pkg 714e9978244eb65bc57b7c4fac5a0430 -C1-1-1.x86_64@available
#>rule pkg 714e9978244eb65bc57b7c4fac5a0430 -Y-1-1.x86_64@available
#>rule pkg 86df5aaf92e5ea31f8efedf442fd431e -B1-1-1.x86_64@available
#>rule pkg 86df5aaf92e5ea31f8efedf442fd431e -C2-1-1.x86_64@available
#>rule pkg 86df5aaf92e5ea31f8efedf442fd431e -Y-1-1.x86_64@available
#>rule pkg 87928b09a90963097188f62d00bfb3d9 -B2-1-1.x86_64@available
#>rule pkg 87928b09a90963097188f62d00bfb3d9 -D1-1-1.x86_64@available
#>rule pkg 87928b09a90963097188f62d00bfb3d9 -Y-1-1.x86_64@available
#>rule pkg 92d66d3ad45eaa7d443a05245c9a0eaf -A2-1-1.x86_64@available
#>rule pkg 92d66d3ad45eaa7d443a05245c9a0eaf -C1-1-1.x86_64@available
#>rule pkg 92d66d3ad45eaa7d443a05245c9a0eaf -Y-1-1.x86_64@available
#>rule pkg b5f410efacf9eb0b2b725e3e7b7d6e93 -B1-1-1.x86_64@available
#>rule pkg b5f410efacf9eb0b2b725e3e7b7d6e93 -D2-1-1.x86_64@available
#>rule pkg b5f410efacf9eb0b2b725e3e7b7d6e93 -Y-1-1.x86_64@available
#>rule pkg c450fe9f8321047ee2cd1e6fc9f34df6 -A2-1-1.x86_64@available
#>rule pkg c450fe9f8321047ee2cd1e6fc9f34df6 -C2-1-1.x86_64@available
#>rule pkg c450fe9f8321047ee2cd1e6fc9f34df6 -Y-1-1.x86_64@available
#>rule pkg da9eb4921698e87f788a89bd25cf75b8 -A1-1-1.x86_64@available
#>rule pkg da9eb4921698e87f788a89bd25cf75b8 -C2-1-1.x86_64@available
#>rule pkg da9eb4921698e87f788a89bd25cf75b8 -Y-1-1.x86_64@available
#>rule pkg dfd799532ba714fd139acf879992c2de -A2-1-1.x86_64@available
#>rule pkg dfd799532ba714fd139acf879992c2de -D2-1-1.x86_64@available
#>rule pkg dfd799532ba714fd139acf879992c2de -Y-1-1.x86_64@available
#>rule pkg eab097949c9719f1e7e196241c6ec0f4 -B2-1-1.x86_64@available
#>rule pkg eab097949c9719f1e7e196241c6ec0f4 -D2-1-1.x86_64@available
#>rule pkg eab097949c9719f1e7e196241c6ec0f4 -Y-1-1.x86_64@available
#>rule pkg ede76598bf66f6be0b1fa3439c97a71b -A1-1-1.x86_64@available
#>rule pkg ede76598bf66f6be0b1fa3439c97a71b -D1-1-1.x86_64@available
#>rule pkg ede76598bf66f6be0b1fa3439c97a71b -Y-1-1.x86_64@available
