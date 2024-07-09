repo @System 0 testtags <inline>
#>=Pkg: krb5-libs 1.18.2 22.el8_7 x86_64
#>=Prv: krb5-libs(x86-64) = 1.18.2-22.el8_7

repo available 0 testtags <inline>
#>=Pkg: ipa-client 4.9.11 5.el8 x86_64
#>=Req: krb5-pkinit-openssl
#>
#>=Pkg: krb5-libs 1.18.2 25.el8_8 x86_64
#>=Prv: krb5-libs(x86-64) = 1.18.2-25.el8_8
#>
#>=Pkg: krb5-pkinit 1.18.2 25.el8_8 x86_64
#>=Req: krb5-libs(x86-64)
#>=Prv: krb5-pkinit-openssl = 1.18.2-25.el8_8
#>
#>=Pkg: krb5-pkinit 1.18.2 22.el8_7 x86_64
#>=Req: krb5-libs(x86-64)
#>=Prv: krb5-pkinit-openssl = 1.18.2-22.el8_7

system x86_64 rpm @System
solverflags focusnew

job install pkg ipa-client-4.9.11-5.el8.x86_64@available
result transaction,problems <inline>
#>install ipa-client-4.9.11-5.el8.x86_64@available
#>install krb5-pkinit-1.18.2-25.el8_8.x86_64@available
