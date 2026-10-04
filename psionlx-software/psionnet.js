// PsionLX Software: Firefox reaches the web through PsionNet, using the proxy
// auto-config file psionnet-connect writes at login. With no PsionNet on the
// network, that file sends everything direct.
pref("network.proxy.type", 2);
pref("network.proxy.autoconfig_url", "file:///home/lx/.psionnet/proxy.pac");
