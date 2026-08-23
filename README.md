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
