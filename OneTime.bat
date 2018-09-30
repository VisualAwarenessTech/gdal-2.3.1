echo vs2017 Directories
if not exist vc141 mkdir vc141
if not exist vc141\\Win32 mkdir vc141\\Win32
if not exist vc141\\x64 mkdir vc141\\x64
if not exist vc141\\Win32\\vs2017_Debug mkdir vc141\\Win32\\vs2017_Debug
if not exist vc141\\Win32\\vs2017_Debug\\gdalplugins mkdir vc141\\Win32\\vs2017_Debug\\gdalplugins
if not exist vc141\\Win32\\vs2017_Release mkdir vc141\\Win32\\vs2017_Release
if not exist vc141\\Win32\\vs2017_Release\\gdalplugins mkdir vc141\\Win32\\vs2017_Release\\gdalplugins
if not exist vc141\\Win32\\bin mkdir vc141\\Win32\\bin
if not exist vc141\\Win32\\bin\\gdalplugins mkdir vc141\\Win32\\bin\\gdalplugins
if not exist vc141\\Win32\\bin\\java mkdir vc141\\Win32\\bin\\java
if not exist vc141\\x64\\vs2017_Debug mkdir vc141\\x64\\vs2017_Debug
if not exist vc141\\x64\\vs2017_Debug\\gdalplugins mkdir vc141\\x64\\vs2017_Debug\\gdalplugins
if not exist vc141\\x64\\vs2017_Release mkdir vc141\\x64\\vs2017_Release
if not exist vc141\\x64\\vs2017_Release\\gdalplugins mkdir vc141\\x64\\vs2017_Release\\gdalplugins
if not exist vc141\\x64\\bin mkdir vc141\\x64\\bin
if not exist vc141\\x64\\bin\\gdalplugins mkdir vc141\\x64\\bin\\gdalplugins
if not exist vc141\\x64\\bin\\java mkdir vc141\\x64\\bin\\java
if not exist vc141\\Win32\\data mkdir vc141\\Win32\\data
if not exist vc141\\x64\\data mkdir vc141\\x64\\data
copy /y data\* vc141\\Win32\\data
copy /y data\* vc141\\x64\\data
