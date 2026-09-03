# 초기 fio 워크로드 스크립트 (2026-02-06 시점)

현재 `test_script/`의 fio 스크립트들로 대체되기 전 판본. `nvmevirt_backup.zip`
에만 남아 있던 것을 VM 폐기 전에 복원했다.

- `fio_write_final.sh` — hot/cold 동기 테스트. 3G 디스크 기준 hot 600M /
  cold 2100M 분할 근거가 주석에 적혀 있다. 나머지 셋의 파라미터 출처.
- `fio_hcwrite.sh` — randwrite 2700M/30G
- `fio_write.sh` — randwrite 2800M/5G
- `fio_write_yeonseo.sh` — heredoc 방식 job 정의, time_based 300s
