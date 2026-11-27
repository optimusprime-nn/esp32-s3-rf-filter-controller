# Installer for the RF controller WeeWX driver:  weectl extension install tools/weewx

from weecfg.extension import ExtensionInstaller


def loader():
    return RFControllerInstaller()


class RFControllerInstaller(ExtensionInstaller):
    def __init__(self):
        super(RFControllerInstaller, self).__init__(
            version='1.0',
            name='rfcontroller',
            description='Reads the ESP32-S3 RF controller weather feed (VEVOR 7-in-1 station).',
            author='optimusprime-nn',
            config={
                'RFController': {
                    'host': '192.168.2.51',
                    'port': '30090',
                    'station': '',
                    'lux_per_wm2': '126.7',
                    'driver': 'user.rfcontroller'}},
            files=[('bin/user', ['bin/user/rfcontroller.py'])]
        )
