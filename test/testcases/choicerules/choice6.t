#Rule #4:
#    !php-fpm-7.2.24-1.noarch [5] (w1)
#    glibc-2.17-325.noarch [2]I (w2)
#    libcrypt-4.1.1-6.noarch [7]
#=> no choice rule for #4
#
repo @System 0 testtags <inline>
#>=Pkg: glibc 2.17 325 noarch
#>=Prv: libcrypt
#>=Pkg: php 5.4.16 48 noarch
repo available 0 testtags <inline>
#>=Pkg: php 7.2.24 1 noarch
#>=Rec: php-fpm = 7.2.24-1
#>=Pkg: php-fpm 7.2.24 1 noarch
#>=Req: libcrypt
#>=Pkg: php-fpm 8.0.13 2 noarch
#>=Req: libcrypt
#>=Pkg: libcrypt 4.1.1 6 noarch
#>=Req: libc
#>=Pkg: glibc 2.28 181 noarch
#>=Prv: libc
system i686 rpm @System
job update all packages
result transaction,problems <inline>
#>install libcrypt-4.1.1-6.noarch@available
#>install php-fpm-7.2.24-1.noarch@available
#>upgrade glibc-2.17-325.noarch@@System glibc-2.28-181.noarch@available
#>upgrade php-5.4.16-48.noarch@@System php-7.2.24-1.noarch@available
