# NVMeVirt

## Introduction

NVMeVirt is a versatile software-defined virtual NVMe device. It is implemented as a Linux kernel module providing the system with a virtual NVMe device of various kinds. Currently, NVMeVirt supports conventional SSDs, NVM SSDs, ZNS SSDs, etc. The device is emulated at the PCI layer, presenting a native NVMe device to the entire system. Thus, NVMeVirt has the capability not only to function as a standard storage device, but also to be utilized in advanced storage configurations, such as NVMe-oF target offloading, kernel bypassing, and PCI peer-to-peer communication.

Further details on the design and implementation of NVMeVirt can be found in the following papers.
- [NVMeVirt: A Versatile Software-defined Virtual NVMe Device (FAST 2023)](https://www.usenix.org/conference/fast23/presentation/kim-sang-hoon)
- [Empowering Storage Systems Research with NVMeVirt: A Comprehensive NVMe Device Emulator (Transactions on Storage 2023)](https://dl.acm.org/doi/full/10.1145/3625006)

Please feel free to contact us at [nvmevirt@gmail.com](mailto:nvmevirt@gmail.com) if you have any questions or suggestions. Also you can raise an issue anytime for bug reports or discussions.

We encourage you to cite our paper at FAST 2023 as follows:
```
@InProceedings{NVMeVirt:FAST23,
  author = {Sang-Hoon Kim and Jaehoon Shim and Euidong Lee and Seongyeop Jeong and Ilkueon Kang and Jin-Soo Kim},
  title = {{NVMeVirt}: A Versatile Software-defined Virtual {NVMe} Device},
  booktitle = {Proceedings of the 21st USENIX Conference on File and Storage Technologies (USENIX FAST)},
  address = {Santa Clara, CA},
  month = {February},
  year = {2023},
}
```


## Installation

### Linux kernel requirement

The recommended Linux kernel version is v5.15.x and higher (tested on Linux vanilla kernel v5.15.37 and Ubuntu kernel v5.15.0-58-generic).

### Reserving physical memory

A part of the main memory should be reserved for the storage of the emulated NVMe device. To reserve a chunk of physical memory, add the following option to `GRUB_CMDLINE_LINUX` in `/etc/default/grub` as follows:

```bash
GRUB_CMDLINE_LINUX="memmap=64G\\\$128G"
```

This example will reserve 64GiB of physical memory chunk (out of the total 192GiB physical memory) starting from the 128GiB memory offset. You may need to adjust those values depending on the available physical memory size and the desired storage capacity.

After changing the `/etc/default/grub` file, you are required to run the following commands to update `grub` and reboot your system.

```bash
$ sudo update-grub
$ sudo reboot
```

### Compiling `nvmevirt`

Please download the latest version of `nvmevirt` from Github:

```bash
$ git clone https://github.com/snu-csl/nvmevirt
```

`nvmevirt` is implemented as a Linux kernel module. Thus, the kernel headers should be installed in the `/lib/modules/$(shell uname -r)` directory to compile `nvmevirt`.

Currently, you need to select the target device type by manually editing the `Kbuild`. You may find the following lines in the `Kbuild`, which imply that NVMeVirt is currently configured for emulating NVM(Non-Volatile Memory) SSD (such as Intel Optane SSD). You may uncomment other one to change the target device type. Note that you can select one device type at a time.

```Makefile
# Select one of the targets to build
CONFIG_NVMEVIRT_NVM := y
#CONFIG_NVMEVIRT_SSD := y
#CONFIG_NVMEVIRT_ZNS := y
#CONFIG_NVMEVIRT_KV := y
```

You may find the detailed configuration parameters for conventional SSD and ZNS SSD from `ssd_config.h`.

Build the kernel module by running the `make` command in the `nvmevirt` source directory.
```bash
$ make
make -C /lib/modules/5.15.37/build M=/path/to/nvmev modules
make[1]: Entering directory '/path/to/linux-5.15.37'
  CC [M]  /path/to/nvmev/main.o
  CC [M]  /path/to/nvmev/pci.o
  CC [M]  /path/to/nvmev/admin.o
  CC [M]  /path/to/nvmev/io.o
  CC [M]  /path/to/nvmev/dma.o
  CC [M]  /path/to/nvmev/simple_ftl.o
  LD [M]  /path/to/nvmev/nvmev.o
  MODPOST /path/to/nvmev/Module.symvers
  CC [M]  /path/to/nvmev/nvmev.mod.o
  LD [M]  /path/to/nvmev/nvmev.ko
  BTF [M] /path/to/nvmev/nvmev.ko
make[1]: Leaving directory '/path/to/linux-5.15.37'
$
```

### Using `nvmevirt`

`nvmevirt` is configured to emulate the NVM SSD by default. You can attach an emulated NVM SSD in your system by loading the `nvmevirt` kernel module as follows:

```bash
$ sudo insmod ./nvmev.ko \
  memmap_start=128G \       # e.g., 1M, 4G, 8T
  memmap_size=64G   \       # e.g., 1M, 4G, 8T
  cpus=7,8                  # List of CPU cores to process I/O requests (should have at least 2)
```

In the above example, `memmap_start` and `memmap_size` indicate the relative offset and the size of the reserved memory, respectively. Those values should match the configurations specified in the `/etc/default/grub` file shown earlier. In addition, the `cpus` option specifies the id of cores on which I/O dispatcher and I/O worker threads run. You have to specify at least two cores for this purpose: one for the I/O dispatcher thread, and one or more cores for the I/O worker thread(s).

It is highly recommended to use the `isolcpus` Linux command-line configuration to avoid schedulers putting tasks on the CPUs that NVMeVirt uses:

```bash
GRUB_CMDLINE_LINUX="memmap=64G\\\$128G isolcpus=7,8"
```

When you are successfully load the `nvmevirt` module, you can see something like these from the system message.

```log
$ sudo dmesg
[  144.812917] nvme nvme0: pci function 0001:10:00.0
[  144.812975] NVMeVirt: Successfully created virtual PCI bus (node 1)
[  144.813911] NVMeVirt: nvmev_proc_io_0 started on cpu 7 (node 1)
[  144.813972] NVMeVirt: Successfully created Virtual NVMe device
[  144.814032] NVMeVirt: nvmev_dispatcher started on cpu 8 (node 1)
[  144.822075] nvme nvme0: 48/0/0 default/read/poll queues
```

If you encounter a kernel panic in `__pci_enable_msix()` or in `nvme_hwmon_init()` during `insmod`, it is because the current implementation of `nvmevirt` is not compatible with IOMMU. In this case, you can either turn off Intel VT-d or IOMMU in BIOS, or disable the interrupt remapping using the grub option as shown below:

```bash
GRUB_CMDLINE_LINUX="memmap=64G\\\$128G intremap=off"
```

Now the emulated `nvmevirt` device is ready to be used as shown below. The actual device number (`/dev/nvme0`) can vary depending on the number of real NVMe devices in your system.


```bash
$ ls -l /dev/nvme*
crw------- 1 root root 242, 0 Feb 22 14:13 /dev/nvme0
brw-rw---- 1 root disk 259, 5 Feb 22 14:13 /dev/nvme0n1
```

## 테스트 스크립트 (`test_script/`)

SLC 캐시 구현을 실제로 돌려보기 위한 스크립트 모음. 전부 저장소 루트에서
실행한다(`./test_script/xxx.sh`).

### 실행 환경 분기

스크립트들은 `hostname`으로 두 환경을 구분한다.

| | `research-pc` | 그 외 |
|---|---|---|
| 저장소 경로 | `/home/wei/nvmevirt` | `/home/wei/chlab/nvmevirt` |
| 디바이스 | `/dev/nvme1n1` | `/dev/nvme0n1` |
| memmap | `memmap_start=4G`, `memmap_size=12G` | `memmap_start=4G`, `memmap_size=3G` |

`insmod.sh`의 `memmap_start`/`memmap_size`는 grub의 `memmap=` 예약 영역과
반드시 일치해야 한다. 안 맞으면 `e820` 검사에 걸려 insmod가 실패한다.
다른 머신에서 쓸 거면 이 두 값과 위 경로부터 확인할 것.

### 모듈 로드/해제

| 스크립트 | 하는 일 |
|---|---|
| `start.sh` | `rmmod` → `insmod` → `mount` 한 번에. 보통 이것만 쓰면 됨 |
| `end.sh` | `umount` → `rmmod` → `make clean` |
| `insmod.sh` | `nvmev.ko`를 `cpus=1,2`로 로드. 이미 올라가 있으면 먼저 내림 |
| `rmmod.sh` | 모듈 내림 |
| `mount.sh` | `mkfs.ext4` 후 `test_script/mnt`에 마운트, 소유권을 `wei`로 |
| `umount.sh` | 언마운트 |
| `cleaner.sh` | 마운트 지점 내용물 삭제(재측정 전 초기화용) |

`mount.sh`는 매번 `mkfs.ext4 -F`를 하므로 마운트 지점의 기존 데이터는 사라진다.

### 워크로드

| 스크립트 | 패턴 | 용도 |
|---|---|---|
| `fio_write_seq.sh` | 4K 순차 쓰기, 2700M | SLC 캐시를 채워서 migration을 유발 |
| `fio_write_random.sh` | 4K 랜덤 쓰기, 4000 IOPS 제한, 300초 | invalidation을 꾸준히 발생시켜 GC 유발 |
| `fio_read.sh` | 4K 랜덤 읽기, io_uring, 20초 | SLC/TLC 읽기 지연시간 차이 확인 |
| `fio_write_cb.sh` | hot 450M / cold 2250M 분리 | victim 선정 정책 비교(3G 구성) |
| `fio_write_server.sh` | hot 1G / cold 9G 분리 | 위와 동일, 12G 구성용 |

`fio_write_cb.sh`와 `fio_write_server.sh`는 hot/cold 영역을 나눠 서로 다른
`rate_iops`로 쓰기 때문에 라인마다 age와 valid page 수가 벌어진다. cost-benefit
victim 선정이 greedy보다 유리한지 보려면 이 두 개를 쓴다.

### 빌드 변형

victim 선정 정책은 빌드 타임에 정해진다.

| 명령 | 정책 |
|---|---|
| `make` | greedy (기본) |
| `make cb` | cost-benefit (`-DCB`) |
| `make rd` | 랜덤 (`-DRD`) |

SLC 캐시 자체는 `ssd_config.h`의 `ENABLE_SLC_CACHE`로 켜고 끈다.

### 기타

| 스크립트 | 하는 일 |
|---|---|
| `download.sh` | 저장소를 새로 clone (`chlab`에서 실행) |
| `upload.sh` | `git add`/`commit`/`push`. 메시지를 안 주면 현재 시각으로 자동 생성 |

## Contributing
When contributing to this repository, please first discuss the change you wish to make via [issues](https://github.com/snu-csl/nvmevirt/issues) or email(nvmevirt@gmail.com) before making a change.

### Pull Requests
1. Create a personal fork of the project on Github.
2. Clone the fork on your local machine.
3. Implement/fix your feature, comment your code.
4. Follow the code style of this project, including indentation.
5. Run tests using [nvmev-evaluation](https://github.com/snu-csl/nvmev-evaluation).
6. From your fork open a pull request in our `main` branch!
7. Please wait for the maintainer's review.


## License

NVMeVirt is offered under the terms of the GNU General Public License version 2 as published by the Free Software Foundation. More information about this license can be found [here](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).

Priority queue implementation [`pqueue/`](pqueue/) is offered under the terms of the BSD 2-clause license (GPL-compatible). (Copyright (c) 2014, Volkan Yazıcı <volkan.yazici@gmail.com>. All rights reserved.)


