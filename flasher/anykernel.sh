# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel setup
# begin properties
properties() { '
kernel.string=
do.devicecheck=1
do.modules=0
do.cleanup=1
do.cleanuponabort=0
device.name1=z2_plus
device.name2=z2131
device.name3=Z2
device.name4=z2
device.name5=Z2131
'; } # end properties

# shell variables
block=/dev/block/bootdevice/by-name/boot;
is_slot_device=0;
ramdisk_compression=auto;


## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. tools/ak3-core.sh;

## AnyKernel install
dump_boot;

# mount vendor
mount -o rw,remount -t auto /vendor >/dev/null;

# begin userspace changes
# backup files
if [ ! -f /vendor/ect/init/hw/init.qcom.power.rc.bkp ]; then
	cp -rpf /vendor/ect/init/hw/init.qcom.power.rc /vendor/ect/init/hw/init.qcom.power.rc.bkp;
fi
if [ ! -f /vendor/etc/thermal-engine.conf.bkp ]; then
	cp -rpf /vendor/etc/thermal-engine.conf /vendor/etc/thermal-engine.conf.bkp;
fi
if [ ! -f /vendor/etc/perf/perfboostsconfig.xml.bkp ]; then
	cp -rpf /vendor/etc/perf/perfboostsconfig.xml /vendor/etc/perf/perfboostsconfig.xml.bkp;
fi
if [ ! -f /vendor/etc/powerhint.xml.bkp ]; then
	cp -rpf /vendor/etc/powerhint.xml /vendor/etc/powerhint.xml.bkp;
fi
if [ ! -f /vendor/bin/thermal-engine.bkp ]; then
	cp -rpf /vendor/bin/thermal-engine /vendor/bin/thermal-engine.bkp;
fi

# replace files
cp -rpf $patch/init.qcom.power.rc /vendor/etc/init/hw/init.qcom.power.rc;
cp -rpf $patch/thermal-engine.conf /vendor/etc/thermal-engine.conf;
cp -rpf $patch/perfboostsconfig.xml /vendor/etc/perf/perfboostsconfig.xml;
cp -rpf $patch/powerhint.xml /vendor/etc/powerhint.xml;
cp -rpf $patch/thermal-engine /vendor/bin/thermal-engine;

# set up permissions
chmod 0644 /vendor/etc/init/hw/init.qcom.power.rc;
chmod 0644 /vendor/etc/thermal-engine.conf;
chmod 0644 /vendor/etc/perf/perfboostsconfig.xml;
chmod 0644 /vendor/etc/powerhint.xml;
chmod 0777 /vendor/bin/thermal-engine;
# end userspace changes

write_boot;
## end install