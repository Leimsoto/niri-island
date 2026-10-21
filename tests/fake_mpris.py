# A pretend media player on the session bus (MPRIS), always "playing", for
# testing the island without real music:  python3 tests/fake_mpris.py [title] [artist] &
import sys
from gi.repository import Gio, GLib
title = sys.argv[1] if len(sys.argv) > 1 else "Prueba de espectro"
artist = sys.argv[2] if len(sys.argv) > 2 else "niri-island"
XML = """<node>
<interface name="org.mpris.MediaPlayer2">
  <property name="Identity" type="s" access="read"/>
  <property name="CanRaise" type="b" access="read"/>
  <method name="Raise"/>
</interface>
<interface name="org.mpris.MediaPlayer2.Player">
  <property name="PlaybackStatus" type="s" access="read"/>
  <property name="Metadata" type="a{sv}" access="read"/>
  <property name="Position" type="x" access="read"/>
  <property name="CanPlay" type="b" access="read"/>
  <property name="CanPause" type="b" access="read"/>
  <property name="CanGoNext" type="b" access="read"/>
  <property name="CanGoPrevious" type="b" access="read"/>
  <property name="CanSeek" type="b" access="read"/>
  <method name="PlayPause"/><method name="Play"/><method name="Pause"/><method name="Next"/><method name="Previous"/>
</interface></node>"""
props = {
    "Identity": GLib.Variant("s", "Prueba"), "CanRaise": GLib.Variant("b", False),
    "PlaybackStatus": GLib.Variant("s", "Playing"), "Position": GLib.Variant("x", 0),
    "Metadata": GLib.Variant("a{sv}", {"mpris:trackid": GLib.Variant("o", "/t/1"), "xesam:title": GLib.Variant("s", title),
                                         "xesam:artist": GLib.Variant("as", [artist]), "mpris:length": GLib.Variant("x", 600000000)}),
    "CanPlay": GLib.Variant("b", True), "CanPause": GLib.Variant("b", True), "CanGoNext": GLib.Variant("b", False),
    "CanGoPrevious": GLib.Variant("b", False), "CanSeek": GLib.Variant("b", False),
}
node = Gio.DBusNodeInfo.new_for_xml(XML)
def on_bus(conn, name):
    for iface in node.interfaces:
        conn.register_object("/org/mpris/MediaPlayer2", iface,
                             lambda *a: a[-1].return_value(None),
                             lambda c, s, p, i, prop: props.get(prop), None)
Gio.bus_own_name(Gio.BusType.SESSION, "org.mpris.MediaPlayer2.niriislandtest", 0, on_bus, None, None)
GLib.MainLoop().run()
