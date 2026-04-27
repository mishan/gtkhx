Summary: GtkHx is a GTK+ version of Hx, a UNIX Hotline Client.
Name: gtkhx
Version: 0.9.4
Release: 1
Group: Applications
Copyright: GPL
Packager: misha@nasledov.com
URL: http://gtkhx.sourceforge.net/
Source0: http://gtkhx.sourceforge.net/files/gtkhx-0.9.4.tar.gz
#Provides: none
Requires: gtk+, glib, gdk-pixbuf, openssl, zlib
#Conflicts: none
BuildRoot: /tmp/gtkhx-0.9.4
%Description
GtkHx is a GTK+ version of Hx, a UNIX Hotline Client.
%Prep
%setup
%Build
./configure --prefix=/usr --enable-pixbuf --enable-cipher --enable-compress
make
%Install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT/usr/bin/
mkdir -p $RPM_BUILD_ROOT/usr/share/gtkhx/sounds
make install prefix=$RPM_BUILD_ROOT/usr

%clean
rm -rf $RPM_BUILD_ROOT
%files
%defattr(-,root,root)
/usr/bin/gtkhx
/usr/share/gtkhx/icons.rsrc
/usr/share/gtkhx/sounds/chatinvite.aiff
/usr/share/gtkhx/sounds/chatpost.aiff
/usr/share/gtkhx/sounds/error.aiff
/usr/share/gtkhx/sounds/filedone.aiff
/usr/share/gtkhx/sounds/join.aiff
/usr/share/gtkhx/sounds/logged-in.aiff
/usr/share/gtkhx/sounds/message.aiff
/usr/share/gtkhx/sounds/newspost.aiff
/usr/share/gtkhx/sounds/part.aiff
%doc README COPYING ChangeLog BUGS AUTHORS TODO DOCUMENTATION
