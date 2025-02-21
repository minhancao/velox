===========================
AsyncDataCache (File Cache)
===========================

Background
----------
Velox provides a transparent file cache (AsyncDataCache) to accelerate table scans operators through hot data reuse and prefetch algorithms. 
The file cache is integrated with the memory system to achieve dynamic memory sharing between the file cache and query memory. 
When a query fails to allocate memory, we retry the allocation by shrinking the file cache. 
Therefore, the file cache size is automatically adjusted in response to the query memory usage change. 
See `Memory Management - Velox Documentation <https://facebookincubator.github.io/velox/develop/memory.html>`_  
for more information about Velox's file cache.

Configuration Properties
------------------------
See `Configuration Properties <../configs.rst>`_ for AsyncDataCache related configuration properties.

=========
SSD Cache
=========

Background
----------
The in-memory file cache (AsyncDataCache) is configured to use SSD when provided.
The SSD serves as an extension for the AsyncDataCache.
This helps mitigate the number of reads from slower storage.

Configuration Properties
------------------------
See `Configuration Properties <../configs.rst>`_ for SSD Cache related configuration properties.

Metrics
-------
There are SSD cache relevant metrics that Velox emits during query execution and runtime. 
See `Debugging Metrics <./debugging/metrics.rst>`_ and `Monitoring Metrics <../monitoring/metrics.rst>`_ for more details.


Setup with btrfs filesystem
---------------------------
Multiple factors contribute to utilizing the SSD cache effectively. 
One of them is choosing a file system that allows direct writes for best performance.
Btrfs is a recommended file system due to its built-in data compression, 
support for O_DIRECT writes, and the ability to perform asynchronous discard operations. 
These features combine to enhance storage efficiency, improve performance, and optimize disk management.

NOTE: Commands below were ran successfully for worker machines of Amazon EC2 r6 instances with CentOS Stream 9.


.. code-block:: bash

    # Installs the centos-release-hyperscale-experimental module and other necessary packages.
    # https://sigs.centos.org/hyperscale/content/repositories/experimental/
    # It will also upgrade the kernel to the supported version for btrfs installation.
    hostnamectl
    sudo dnf -y install centos-release-hyperscale-experimental
    sudo dnf --disablerepo=* --enablerepo=centos-hyperscale,centos-hyperscale-experimental -y update --allowerasing
    sudo dnf -y install kernel-modules-extra
    # Restart worker machine to have the new Kernel version take into effect.
    sudo shutdown -r now || true


.. code-block:: bash

    # The systemd packages need to be updated to match with the new updated kernel.
    sudo dnf -y install systemd-networkd systemd-boot


.. code-block:: bash

    # Install the btrfs packages.
    hostnamectl
    sudo yum -y install btrfs-progs
    echo "Checking /proc/filesystems for btrfs support..."
    if ! grep -q btrfs /proc/filesystems; then
        echo "Btrfs is not supported by the kernel."
        exit 1
    fi
    echo "Btrfs is supported by the kernel."


.. code-block:: bash

    # If btrfs is successfully supported by the kernel, mount btrfs onto a disk and directory path.
    sudo lsblk -d -o NAME | tail -n +2
    # Only install btrfs onto a disk that is not EBS (EBS holds the OS).
    disk_names=( $(sudo lsblk -d -o NAME | tail -n +2) )
    for disk in "${disk_names[@]}"; do
        echo "Checking disk: $disk"
        # If the disk is an Amazon EC2 NVMe Instance Storage volume, then install btrfs onto that disk
        if sudo fdisk -l "/dev/$disk" | grep -q "Amazon EC2 NVMe Instance Storage"; then
            echo "Disk $disk is an Amazon EC2 NVMe Instance Storage"
            sudo mkfs.btrfs /dev/$disk
            sudo mount -t btrfs /dev/$disk /home/centos/presto/async_data_cache
            sudo echo "/dev/$disk /home/centos/presto/async_data_cache auto noatime 0 0" | sudo tee -a /etc/fstab
            sudo lsblk -f
            break
        else
            echo "Disk $disk is not an Amazon EC2 NVMe Instance Storage volume"
        fi
    done
