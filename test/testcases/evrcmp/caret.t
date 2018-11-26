repo system 0 empty
repo available 0 testtags <inline>
#>=Pkg: A1 1.0 0
#>=Pkg: A2 1.0^ 0
system i686 rpm system

evrcmp 1.0~rc1 1.0~rc1
evrcmp 1.0~rc1 1.0
evrcmp 1.0 1.0~rc1
evrcmp 1.0~rc1 1.0~rc2
evrcmp 1.0~rc2 1.0~rc1
evrcmp 1.0~rc1~git123 1.0~rc1~git123
evrcmp 1.0~rc1~git123 1.0~rc1
evrcmp 1.0~rc1 1.0~rc1~git123

evrcmp 1.0^ 1.0^
evrcmp 1.0^ 1.0
evrcmp 1.0 1.0^
evrcmp 1.0^git1 1.0^git1
evrcmp 1.0^git1 1.0
evrcmp 1.0 1.0^git1
evrcmp 1.0^git1 1.0^git2
evrcmp 1.0^git2 1.0^git1
evrcmp 1.0^git1 1.01
evrcmp 1.01 1.0^git1
evrcmp 1.0^20160101 1.0^20160101
evrcmp 1.0^20160101 1.0.1
evrcmp 1.0.1 1.0^20160101
evrcmp 1.0^20160101^git1 1.0^20160101^git1
evrcmp 1.0^20160102 1.0^20160101^git1
evrcmp 1.0^20160101^git1 1.0^20160102

evrcmp 1.0~rc1^git1 1.0~rc1^git1
evrcmp 1.0~rc1^git1 1.0~rc1
evrcmp 1.0~rc1 1.0~rc1^git1
evrcmp 1.0^git1~pre 1.0^git1~pre
evrcmp 1.0^git1 1.0^git1~pre
evrcmp 1.0^git1~pre 1.0^git1
evrcmp 1.0^1 1.0~1
evrcmp 1.0~1 1.0^1

result jobs <inline>
#>job noop provides 1.0~rc1 = 1.0~rc1
#>job noop provides 1.0~rc1 < 1.0
#>job noop provides 1.0 > 1.0~rc1
#>job noop provides 1.0~rc1 < 1.0~rc2
#>job noop provides 1.0~rc2 > 1.0~rc1
#>job noop provides 1.0~rc1~git123 = 1.0~rc1~git123
#>job noop provides 1.0~rc1~git123 < 1.0~rc1
#>job noop provides 1.0~rc1 > 1.0~rc1~git123
#>job noop provides 1.0^ = 1.0^
#>job noop provides 1.0^ > 1.0
#>job noop provides 1.0 < 1.0^
#>job noop provides 1.0^git1 = 1.0^git1
#>job noop provides 1.0^git1 > 1.0
#>job noop provides 1.0 < 1.0^git1
#>job noop provides 1.0^git1 < 1.0^git2
#>job noop provides 1.0^git2 > 1.0^git1
#>job noop provides 1.0^git1 < 1.01
#>job noop provides 1.01 > 1.0^git1
#>job noop provides 1.0^20160101 = 1.0^20160101
#>job noop provides 1.0^20160101 < 1.0.1
#>job noop provides 1.0.1 > 1.0^20160101
#>job noop provides 1.0^20160101^git1 = 1.0^20160101^git1
#>job noop provides 1.0^20160102 > 1.0^20160101^git1
#>job noop provides 1.0^20160101^git1 < 1.0^20160102
#>job noop provides 1.0~rc1^git1 = 1.0~rc1^git1
#>job noop provides 1.0~rc1^git1 > 1.0~rc1
#>job noop provides 1.0~rc1 < 1.0~rc1^git1
#>job noop provides 1.0^git1~pre = 1.0^git1~pre
#>job noop provides 1.0^git1 > 1.0^git1~pre
#>job noop provides 1.0^git1~pre < 1.0^git1
#>job noop provides 1.0^1 > 1.0~1
#>job noop provides 1.0~1 < 1.0^1
