feature complex_deps
repo appstream 0 testtags <inline>
#>=Pkg: xorg-x11-server-Xorg 1.20.11 18.el9 noarch
#>=Req: missing-req
#>=Pkg: pass 1.7.4 6.el9 noarch
#>=Rec: xclip <IF> (xorg-x11-server-Xorg <ELSE> wl-clipboard)
repo @System 0 empty
system unset * @System
job install pkg pass-1.7.4-6.el9.noarch@appstream
result transaction,problems <inline>
#>install pass-1.7.4-6.el9.noarch@appstream
