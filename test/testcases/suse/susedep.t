genid dep modalias(bar:baz)
genid susedep solvable:supplements
result genid <inline>
#>genid  1: genid lit namespace:modalias
#>genid  2: genid lit bar:baz
#>genid  3: genid op <NAMESPACE>
#>genid dep namespace:modalias(bar:baz)
nextjob

genid dep modalias(foo:bar:baz)
genid susedep solvable:supplements
result genid <inline>
#>genid  1: genid lit foo
#>genid  2: genid lit namespace:modalias
#>genid  3: genid lit bar:baz
#>genid  4: genid op <NAMESPACE>
#>genid  5: genid op &
#>genid dep foo & namespace:modalias(bar:baz)
nextjob

genid dep kernel & modalias(foo:bar:baz)
genid susedep solvable:supplements
result genid <inline>
#>genid  1: genid lit kernel
#>genid  2: genid lit foo
#>genid  3: genid lit namespace:modalias
#>genid  4: genid lit bar:baz
#>genid  5: genid op <NAMESPACE>
#>genid  6: genid op &
#>genid  7: genid op &
#>genid dep kernel & foo & namespace:modalias(bar:baz)
nextjob

genid dep filesystem(foo:bar)
genid susedep solvable:supplements
result genid <inline>
#>genid  1: genid lit namespace:filesystem
#>genid  2: genid lit foo:bar
#>genid  3: genid op <NAMESPACE>
#>genid dep namespace:filesystem(foo:bar)
nextjob

genid dep packageand(foo:bar)
genid susedep solvable:supplements
result genid <inline>
#>genid  1: genid lit foo
#>genid  2: genid lit bar
#>genid  3: genid op &
#>genid dep foo & bar
nextjob

genid dep packageand(foo:pattern:bar)
genid susedep solvable:supplements
result genid <inline>
#>genid  1: genid lit foo
#>genid  2: genid lit pattern:bar
#>genid  3: genid op &
#>genid dep foo & pattern:bar
nextjob

genid dep otherproviders(foo:bar)
genid susedep solvable:conflicts
result genid <inline>
#>genid  1: genid lit namespace:otherproviders
#>genid  2: genid lit foo:bar
#>genid  3: genid op <NAMESPACE>
#>genid dep namespace:otherproviders(foo:bar)
nextjob

genid dep foo:/bin/bar
genid susedep solvable:provides
result genid <inline>
#>genid  1: genid lit namespace:splitprovides
#>genid  2: genid lit foo
#>genid  3: genid lit /bin/bar
#>genid  4: genid op +
#>genid  5: genid op <NAMESPACE>
#>genid dep namespace:splitprovides(foo + /bin/bar)
nextjob

genid dep locale(de)
genid susedep solvable:provides
result genid <inline>
#>genid  1: genid lit namespace:language
#>genid  2: genid lit de
#>genid  3: genid op <NAMESPACE>
#>genid dep namespace:language(de)
nextjob

genid dep locale(foo:de;en)
genid susedep solvable:provides
result genid <inline>
#>genid  1: genid lit foo
#>genid  2: genid lit namespace:language
#>genid  3: genid lit de
#>genid  4: genid op <NAMESPACE>
#>genid  5: genid lit namespace:language
#>genid  6: genid lit en
#>genid  7: genid op <NAMESPACE>
#>genid  8: genid op |
#>genid  9: genid op &
#>genid dep foo & (namespace:language(de) | namespace:language(en))
nextjob

genid dep language(de)
genid susedep solvable:supplements
result genid <inline>
#>genid  1: genid lit namespace:language
#>genid  2: genid lit de
#>genid  3: genid op <NAMESPACE>
#>genid dep namespace:language(de)
nextjob

genid dep language(de;en)
genid susedep solvable:supplements
result genid <inline>
#>genid  1: genid lit namespace:language
#>genid  2: genid lit de
#>genid  3: genid op <NAMESPACE>
#>genid  4: genid lit namespace:language
#>genid  5: genid lit en
#>genid  6: genid op <NAMESPACE>
#>genid  7: genid op |
#>genid dep namespace:language(de) | namespace:language(en)

