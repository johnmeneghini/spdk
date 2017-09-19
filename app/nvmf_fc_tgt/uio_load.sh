lsmod | grep lpfc
if [ $? -eq 0 ]; then 
	echo "Unloading lpfc driver..."
	rmmod lpfc
fi
modprobe -v uio_pci_generic
#Attach UIO driver to Emulex Lancer G6 adapter
echo "10df e300" > /sys/bus/pci/drivers/uio_pci_generic/new_id
#Attach UIO driver to Emulex Lancer G5 adapter
echo "10df e200" > /sys/bus/pci/drivers/uio_pci_generic/new_id

echo 2048 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
echo 2048 > /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages
