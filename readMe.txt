-----BEGIN PGP SIGNED MESSAGE-----
Hash: SHA256

# BonDriver_uSUNpTV.dll
2016-02-19, Ver. 0.1.3

This is a BonDriver DLL for uSUNpTV (ISDB-T/S TV Tuner)
C, Visual C++ source code package (MS Visual C++ 2010)



## Files

/
	readMe.txt
	ChangeLog.txt
	sha1sum.txt
	sha1sum.txt.sig
		* gpg --verify sha1sum.txt.sig

src/
	* source code files for Windows programs
	uSUNpTV.sln
		* VS solution file
	em287x.{c,h}
	em287x_priv.h
	tc90522.{c,h}
	mxl136.{c,h}
	tda20142.{c,h}
		* Device operation code (same as Linux ver.)
	*.{c,cpp,h}

src/BonDriver/
	* source code for BonDriver
	BonDriver_uSUNpTV.vcxproj{,.*}
		* VS project file
	*.{c,cpp,h}
	BonDriver.rc



## User Agreement

USE AT YOUR OWN RISK, no warranty

You can ...
* use this package for any purposes.
* redistribute a copy of this package without modifying.
* modify this package.
* redistribute the modified package. You should write the modification clearly.

based on GPLv3



## How to compile and build?

I use the VC++ Express version. (Microsoft Visual C++ 2010 SP1)
You can download from MS site.

Build the "BonDriver_uSUNpTV" project.

(x64 environment setting has NOT been checked YET.)



## End

(c) 2016 trinity19683
signed by "trinity19683.gpg-pub" 2015-12-13
-----BEGIN PGP SIGNATURE-----
Version: GnuPG v1

iQEcBAEBCAAGBQJWyByEAAoJEB0tpC02lUUYDZoIAIxmwK4qog/3QuPbnhWUClA2
WxUklQuGDVmgxbdFBZ0KS+no1kNpk9uIHnAk/cLPJscDuguFyKzS03Lxc1+3EElu
uxyGJzuoI9gn4YcHancas4NBH55DZ1QbM6KCVpFylcxI3uboCIL9/7bE30Y6TPF1
7vc3d8uuC7TqSC6erSwAIECPpRJq+n5BI7hBCkWPNDuMzyMPotOOqqTnDEwDNY1A
ffjDhmoa/1NOw0VUeWLlJgRXsr04thUT6UEmXnKhK+1TD7TiWDtVdfxTuYffWpXH
rrNiQAifh7Q9deLFfWmbFTlJ2EP3k14/Wjgfqama9w2n+rhtYqovxvPPuTPVmPI=
=nPTh
-----END PGP SIGNATURE-----
