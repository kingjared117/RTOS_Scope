# invoke SourceDir generated makefile for app.pem3
app.pem3: .libraries,app.pem3
.libraries,app.pem3: package/cfg/app_pem3.xdl
	$(MAKE) -f E:\test/src/makefile.libs

clean::
	$(MAKE) -f E:\test/src/makefile.libs clean

