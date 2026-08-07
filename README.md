# Mokai

A C++ build system for small projects using toml,
basically you write how your project should be built in a toml file then you use one command 
`mokai-rel run` to run it or `mokai-rel build` to only build

## Quick Install

```bash
curl -fsSL https://raw.githubusercontent.com/ketsebaoteth/mokai/main/get.sh | bash
```

if that doesnt work try manually downloading binaries from release and chmod +x mokai-rel and add it to your path for ease of use

to use get started by 

```
mokai-rel create myapp
```
then cd to myapp then 
```
mokai-rel run
```

example simple mokai.toml
```
[project]
name = "myapp"
version = "0.1.0"
cpp_version = "c++23"

[target.myapp]
type = "executable"
sources = ["src/main.cpp"]
```
