#!/bin/bash

# Enable 2MB pages
echo always > /sys/kernel/mm/transparent_hugepage/enabled
echo always > /sys/kernel/mm/transparent_hugepage/defrag

# Enable 1GB pages
# https://www.kernel.org/doc/html/latest/admin-guide/mm/hugetlbpage.html

# Dynamically reserve 1GB pages (40GBs on node0)
echo 40 > /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages
echo 40 > /sys/devices/system/node/node1/hugepages/hugepages-1048576kB/nr_hugepages

if [ ! -d /mnt/huge ]; then
	sudo mkdir -p /mnt/huge
fi
sudo mount -t hugetlbfs nodev /mnt/huge

# Check how many pages are allocated
echo "2MiB pages: $(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)"
echo "1GiB pages: $(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages)"
