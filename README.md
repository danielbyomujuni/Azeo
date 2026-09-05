# Azeo

Azeo is a fork of [Wine](https://www.winehq.org) with additional patches
for hardware and application support (Moza racing accessories, current
FL Studio releases, and more).

## INTRODUCTION

Azeo is a program which allows running Microsoft Windows programs
(including DOS, Windows 3.x, Win32, and Win64 executables) on Unix.
It consists of a program loader which loads and executes a Microsoft
Windows binary, and a library (called Winelib) that implements Windows
API calls using their Unix, X11 or Mac equivalents.  The library may also
be used for porting Windows code into native Unix executables.

Azeo, like the Wine project it is based on, is free software, released
under the GNU LGPL; see the file LICENSE for the details.


## QUICK START

From the top-level directory of the Azeo source (which contains this file),
run:

```
./configure
make
```

Then either install Azeo:

```
make install
```

Or run Azeo directly from the build directory:

```
./azeo notepad
```

Run programs as `azeo program`. For more information and problem
resolution, read the rest of this file, the Azeo man page, and
especially the wealth of information found at https://www.winehq.org.


## REQUIREMENTS

To compile and run Azeo, you must have one of the following:

- Linux version 2.6.22 or later
- FreeBSD 12.4 or later
- Solaris x86 9 or later
- NetBSD-current
- macOS 10.15 or later

As Azeo requires kernel-level thread support to run, only the operating
systems mentioned above are supported.  Other operating systems which
support kernel threads may be supported in the future.

**FreeBSD info**:
  See https://wiki.freebsd.org/Wine for more information.

**Solaris info**:
  You will most likely need to build Azeo with the GNU toolchain
  (gcc, gas, etc.). Warning : installing gas does *not* ensure that it
  will be used by gcc. Recompiling gcc after installing gas or
  symlinking cc, as and ld to the gnu tools is said to be necessary.

**NetBSD info**:
  Make sure you have the USER_LDT, SYSVSHM, SYSVSEM, and SYSVMSG options
  turned on in your kernel.

**macOS info**:
  You need Xcode/Xcode Command Line Tools or Apple cctools.

**Supported file systems**:
  Azeo should run on most file systems. A few compatibility problems
  have also been reported using files accessed through Samba. Also,
  NTFS does not provide all the file system features needed by some
  applications.  Using a native Unix file system is recommended.

**Basic requirements**:
  You need to have the X11 development include files installed
  (called xorg-dev in Debian and libX11-devel in Red Hat).
  Of course you also need make (most likely GNU make).
  You also need flex version 2.5.33 or later and bison.

**Optional support libraries**:
  Configure will display notices when optional libraries are not found
  on your system. See https://gitlab.winehq.org/wine/wine/-/wikis/Building-Wine
  for hints about the packages you should install. On 64-bit
  platforms, you have to make sure to install the 32-bit versions of
  these libraries.


## COMPILATION

To build Azeo, do:

```
./configure
make
```

This will build the program "azeo" and numerous support libraries/binaries.
The program "azeo" will load and run Windows executables.

To see compile configuration options, do `./configure --help`.

For more information, see https://gitlab.winehq.org/wine/wine/-/wikis/Building-Wine


## SETUP

Once Azeo has been built correctly, you can do `make install`; this
will install the azeo executable and libraries, the Azeo man page, and
other needed files.

Don't forget to uninstall any conflicting previous Wine or Azeo
installation first.  Try either `dpkg -r wine` or `rpm -e wine` or
`make uninstall` before installing.

Once installed, you can run the `winecfg` configuration tool. See the
Support area at https://www.winehq.org/ for configuration hints.


## RUNNING PROGRAMS

When invoking Azeo, you may specify the entire path to the executable,
or a filename only.

For example, to run Notepad:

```
azeo notepad            (using the search Path as specified in
azeo notepad.exe         the registry to locate the file)

azeo c:\\windows\\notepad.exe      (using DOS filename syntax)

azeo ~/.wine/drive_c/windows/notepad.exe  (using Unix filename syntax)

azeo notepad.exe readme.txt          (calling program with parameters)
```

Azeo is not perfect, so some programs may crash. If that happens you
will get a crash log that you should attach to your report when filing
a bug.


## GETTING MORE INFORMATION

- **Azeo**: Development of this fork is hosted at
	https://github.com/danielbyomujuni/Azeo — report Azeo-specific
	bugs there.

- **WWW**: A great deal of information about the underlying Wine project is
	available from WineHQ at https://www.winehq.org/ : various Wine Guides,
	application database, bug tracking. This is probably the best starting
	point.

- **FAQ**: The Wine FAQ is located at https://gitlab.winehq.org/wine/wine/-/wikis/FAQ

- **Wiki**: The Wine Wiki is located at https://gitlab.winehq.org/wine/wine/-/wikis/

- **Gitlab**: Upstream Wine development is hosted at https://gitlab.winehq.org

- **Mailing lists**:
	There are several mailing lists for Wine users and developers; see
	https://gitlab.winehq.org/wine/wine/-/wikis/Forums for more
	information.

- **IRC**: Online help is available at channel `#WineHQ` on irc.libera.chat.
