#!/usr/bin/python

import os
import shutil
import socket
import sys


import pkgutil



# initialize the socket callback interface so we have it
# available..
serversocket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
serversocket.bind(('localhost', pkgutil.PORT_CREATOR))
serversocket.listen(16)



class Options:
    def __init__(self):
        self.qtLabel = None
        self.qtVersion = None
        self.packageRoot = None
        self.p4User = "qt"
        self.p4Client = "qt-builder"
        self.startDir = os.getcwd()
        self.p4Resync = True
        
options = Options()

gpl_header = pkgutil.readLicenseHeader(pkgutil.LICENSE_GPL, options.startDir)
commercial_header = pkgutil.readLicenseHeader(pkgutil.LICENSE_COMMERCIAL, options.startDir)
eval_header = pkgutil.readLicenseHeader(pkgutil.LICENSE_EVAL, options.startDir)


class BuildServer:
    def __init__(self, platform, arch):
        self.arch = arch
        self.platform = platform
        
servers = []



def setupServers():
    os.chdir(options.packageRoot)

    mac = BuildServer(pkgutil.PLATFORM_MAC, pkgutil.ARCH_UNIVERSAL)
    mac.host = 'localhost'
    mac.task = options.startDir + "/build_qt_mac.sh"
    servers.append(mac)
    

# Sets up the client spec and performs a complete checkout of the
# tree...
def prepareSourceTree():

    # remove and recreat dir and cd into it...
    if os.path.isdir(options.packageRoot):
        shutil.rmtree(options.packageRoot)
        
    os.makedirs(options.packageRoot)
    os.chdir(options.packageRoot)
    
    # set up the perforce client...
    tmpFile = open("p4spec.tmp", "w")
    tmpFile.write("Root: %s\n" % (options.packageRoot))
    tmpFile.write("Owner: %s\n" % options.p4User)
    tmpFile.write("Client: %s\n" % options.p4Client)
    tmpFile.write("View:\n")
    tmpFile.write("        //depot/qt/%s/...  //%s/qt/...\n" % (options.qtVersion, options.p4Client))
    tmpFile.write("        -//depot/qt/%s/examples/... //qt-builder/qt/examples/...\n" % options.qtVersion)
    tmpFile.write("        -//depot/qt/%s/tests/... //qt-builder/qt/tests/...\n" % options.qtVersion)
    tmpFile.write("        -//depot/qt/%s/demos/... //qt-builder/qt/demos/...\n" % options.qtVersion)
    tmpFile.write("        -//depot/qt/%s/doc/... //qt-builder/qt/doc/...\n" % options.qtVersion)
    tmpFile.write("        -//depot/qt/%s/util/... //qt-builder/qt/util/...\n" % options.qtVersion)
    tmpFile.write("        -//depot/qt/%s/tmake/... //qt-builder/qt/tmake/...\n" % options.qtVersion)
    tmpFile.write("        -//depot/qt/%s/dist/... //qt-builder/qt/dist/...\n" % options.qtVersion)
    tmpFile.write("        -//depot/qt/%s/translations/... //qt-builder/qt/translations/...\n" % options.qtVersion)
    tmpFile.close()
    os.system("p4 -u %s -c %s client -i < p4spec.tmp" % (options.p4User, options.p4Client) );
    os.remove("p4spec.tmp")

    # sync p4 client spec into subdirectory...
    label = ""
    if options.qtLabel:
        label = "@" + options.qtLabel
    pkgutil.debug(" - syncing p4...")
    os.system("p4 -u %s -c %s sync -f //%s/... %s > .p4sync.buildlog" % (options.p4User, options.p4Client, options.p4Client, label))
    os.system("chmod -R a+uw .")



def packageAndSend(server):
    pkgutil.debug("sending to %s, script=%s..." % (server.host, server.task))
    
    os.chdir(options.packageRoot)

    if os.path.isdir("tmptree"):
        shutil.rmtree("tmptree")
    os.makedirs("tmptree")

    print " - setting up gpl subdir..."
    shutil.copytree("qt", "tmptree/gpl");
    pkgutil.expandMacroes("tmptree/gpl", gpl_header)
    
    print " - setting up commercial subdir..."
    shutil.copytree("qt", "tmptree/commercial");
    pkgutil.expandMacroes("tmptree/commercial", commercial_header)
    
    print " - setting up eval subdir..."
    shutil.copytree("qt", "tmptree/eval");
    pkgutil.expandMacroes("tmptree/eval", eval_header)

    if server.platform == pkgutil.PLATFORM_WINDOWS:
        shutil.copy(server.task, "tmptree/task.bat")
    else:
        shutil.copy(server.task, "tmptree/task.sh")

    zipFile = os.path.join(options.packageRoot, "tmp.zip")
    pkgutil.debug(" - compressing...")
    pkgutil.compress(zipFile, os.path.join(options.packageRoot, "tmptree"))
    pkgutil.debug(" - sending to host: %s.." % (server.host))
    pkgutil.sendDataFileToHost(server.host, pkgutil.PORT_SERVER, zipFile)



def waitForResponse():
    packagesRemaining = len(servers)
    pkgutil.debug("Waiting for build server responses...")
    
    while packagesRemaining:
        (sock, (host, port)) = serversocket.accept()
        pkgutil.debug(" - got response from %s:%d" % (host, port))
        match = False
        for server in servers:
            if socket.gethostbyname(server.host) == host:
                dataFile = options.packageRoot + "/" + server.host + ".zip"
                outDir = options.packageRoot + "/" + server.host;
                pkgutil.getDataFile(sock, dataFile)
                pkgutil.debug(" - uncompressing to %s" % outDir)
                pkgutil.uncompress(dataFile, outDir);

                if os.path.isfile(outDir + "/FATAL.ERROR"):
                    print "Build server: %s Failed!!!!" % server.host
                else:
                    print "Build server: %s ok!" % server.host
                    
                match = True
                    
        if match:
            packagesRemaining = packagesRemaining - 1

    


def main():
    for i in range(0, len(sys.argv)):
        arg = sys.argv[i];
        if arg == "--qt-version":
            options.qtVersion = sys.argv[i+1]
        elif arg == "--package-root":
            options.packageRoot = sys.argv[i+1]
        elif arg == "--qt-label":
            options.qtLabel = sys.argv[i+1]
        elif arg == "--no-p4sync":
            options.p4Resync = False
        elif arg == "--verbose":
            pkgutil.VERBOSE = 1

    pkgutil.debug("Options:")
    print "  - Qt Version: " + options.qtVersion
    print "  - Package Root: " + options.packageRoot
    print "  - P4 User: " + options.p4User
    print "  - P4 Client: " + options.p4Client
    print "  - P4 Resync: %s" % options.p4Resync

    if options.p4Resync:
        pkgutil.debug("preparing source tree...")
        prepareSourceTree()

    pkgutil.debug("setting up packages..")
    setupServers()

    for server in servers:
        packageAndSend(server)

    waitForResponse()
    
    


if __name__ == "__main__":
    main()