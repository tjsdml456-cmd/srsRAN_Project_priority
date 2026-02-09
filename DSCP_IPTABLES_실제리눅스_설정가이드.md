# iptables DSCP 적용 가이드 (실제 리눅스 + Open5GS 스크립트)

실제 리눅스에서 Open5GS(스크립트 실행), gNB(YAML), UE(.conf)로 N3 트래픽에 iptables DSCP를 적용하고, outer → inner 복사 → SDAP까지 동작시킨 내용을 정리한 문서입니다.

---

## 목차

1. [환경 정리](#1-환경-정리)
2. [동작 원리](#2-동작-원리)
3. [왜 로컬호스트면 안 되는지](#3-왜-로컬호스트면-안-되는지)
4. [설정 요약](#4-설정-요약)
5. [실제 적용 단계](#5-실제-적용-단계)
6. [gNB 설정: cu_up_ngu_only.yml](#6-gnb-설정-cu_up_ngu_onlyyml)
7. [iptables 규칙 (중요: 소스 IP)](#7-iptables-규칙-중요-소스-ip)
8. [동작 확인](#8-동작-확인)
9. [트러블슈팅](#9-트러블슈팅)
10. [참고: 같은 PC에서 돌릴 때](#10-참고-같은-pc에서-돌릴-때)
11. [같은 PC에서 N3가 한 번 실제 인터페이스를 타게 하기 (UE별 DSCP)](#같은-pc에서-n3가-한-번-실제-인터페이스를-타게-하기-ue별-dscp-적용)
12. [같은 PC에서 UE별 DSCP 시도 요약 (왜 안 됐는지)](#같은-pc에서-ue별-dscp-시도-요약-왜-안-됐는지)

---

## 1. 환경 정리

| 항목 | 내용 |
|------|------|
| **OS** | 실제 리눅스 (WSL 아님) → `xt_DSCP` 사용 가능 |
| **Open5GS** | Docker 아님, bash 스크립트로 `open5gs-*` 실행, 설정: `install/etc/open5gs/*.yaml` |
| **gNB** | YAML 설정으로 실행 (예: `gnb_zmq.yaml`) |
| **UE** | .conf 파일로 실행 (예: 3대) |

---

## 2. 동작 원리

- **iptables**가 N3(UDP 2152) **outer IP 헤더**의 DSCP를 설정함.
- srsRAN 쪽에서 **UDP 게이트웨이**가 `IP_RECVTOS`로 outer ToS를 읽고, **GTP-U 터널**에서 inner IP ToS로 복사한 뒤, **SDAP**가 그 DSCP를 추출함.
- N3 트래픽이 **로컬호스트(127.0.0.x)** 가 아니라 **실제 인터페이스 IP**로 흐를 때만 iptables PREROUTING에 걸림.

---

## 3. 왜 로컬호스트면 안 되는지

- N3가 **127.0.0.x**로 오면 커널이 PREROUTING을 거치지 않고 로컬로 바로 넘김.
- 그래서 `-j DSCP` 규칙이 적용되지 않아 outer ToS가 항상 0으로 남음.
- **해결**: N3를 **실제 인터페이스 IP**로 흐르게 하면 PREROUTING에 걸려 DSCP가 적용됨.

---

## 4. 설정 요약

| 구분 | 할 일 |
|------|--------|
| **Open5GS UPF** | `upf.yaml`에서 `gtpu.server[].address`를 **실제 IP**로 (예: 10.53.1.2 또는 본인 PC의 eth0/wlan IP) |
| **srsRAN gNB** | N3 수신을 **실제 IP**에서 하도록 설정 (전용 파일은 `cu_up_ngu_only.yml` 사용 권장) |
| **iptables** | **N3 패킷의 실제 소스 IP**에 대해 `-s <그 IP>` 로 PREROUTING 규칙 추가 |

---

## 5. 실제 적용 단계

### 5.1 Open5GS UPF (upf.yaml)

- 경로: `$HOME/srsRAN_main/open5gs/install/etc/open5gs/upf.yaml`
- N3용 주소만 실제 IP로:

```yaml
upf:
  gtpu:
    server:
      - address: 10.53.1.2   # 또는 본인 환경의 실제 IP (예: 192.168.0.3)
```

- 변경 후 **UPF 재시작** 필요.

### 5.2 N3가 실제 IP로 흐르게 하기

- **꼭 할 필요 없음.** 기존 인터페이스(eth0, wlan0 등)의 IP만 써도 됨.
  - UPF `upf.yaml`의 `gtpu.server[].address`를 그 IP(예: 192.168.0.3)로 두고,
  - gNB NGU `bind_addr`를 같은 네트워크에서 수신 가능한 주소로 두면 N3는 이미 실제 IP로 흐름.
  - iptables `-s`는 로그에서 확인한 **실제 N3 소스 IP**로 넣으면 됨.

- **(선택)** N3 전용 서브넷(10.53.1.x)을 쓰고 싶을 때만 **dummy** 인터페이스를 만듦:

```bash
sudo ip link add name n3dummy type dummy
sudo ip addr add 10.53.1.2/24 dev n3dummy
sudo ip addr add 10.53.1.3/24 dev n3dummy
sudo ip link set n3dummy up
```

  - 이때는 UPF gtpu를 10.53.1.2, gNB ngu를 10.53.1.3으로 맞추고, iptables는 `-s 10.53.1.2` 사용.

### 5.3 gNB 설정 (gnb_zmq.yaml + N3용 cu_up)

- **gnb_zmq.yaml**에는 보통 **cu_cp**, **ru_sdr**, **cell_cfg**, **log**, **pcap**만 있고 **cu_up** 블록은 없음.
- N3를 실제 IP로 쓰려면 **cu_up.ngu.socket.bind_addr**를 넣어줘야 함. gnb_zmq.yaml은 건드리지 않고, 두 번째 config로 NGU만 넘김.

- **gnb_zmq.yaml은 수정하지 않음.**
- `configs/cu_up_ngu_only.yml`을 **두 번째 config**로 전달해서 실행:

```bash
cd ~/srsRAN_main/srsRAN_Project/build/apps/gnb
sudo ./gnb -c gnb_zmq.yaml -c ../../../configs/cu_up_ngu_only.yml
# 또는 cu_up_ngu_only.yml을 이 디렉터리로 복사해 둔 경우:
sudo ./gnb -c gnb_zmq.yaml -c cu_up_ngu_only.yml
```

- gNB 단일 실행 시 CU-UP 스키마에는 **ngu**만 있음. **e1ap**, **f1u**가 들어 있는 `cu_up.yml`을 두 번째로 주면 `INI was not able to parse cu_up.e1ap.++` 오류가 나므로, **ngu만** 있는 `cu_up_ngu_only.yml`을 써야 함.

### 5.4 N3 주소가 바뀌었을 때

- N3 주소를 127.0.0.x → 실제 IP로 바꾼 뒤에는 **Open5GS 전부 + gNB 재시작** 후 **UE를 한 번 끊었다가 다시 접속**해야, PDU 세션이 새 N3 주소로 잡힘.
- 그래야 N3 패킷이 실제 IP로 흐르고, iptables 규칙에 걸림.

---

## 6. gNB 설정: cu_up_ngu_only.yml

- 경로: `configs/cu_up_ngu_only.yml`
- gNB에 **두 번째 config**로 줄 때 사용. **ngu만** 넣어서 파싱 오류를 피함.

```yaml
# gNB 실행: ./gnb -c gnb_zmq.yaml -c configs/cu_up_ngu_only.yml
cu_up:
  ngu:
    socket:
      -
        bind_addr: 10.53.1.3   # N3 수신용. 실제 환경에 맞게 변경 가능 (예: 192.168.0.x)
```

- `configs/cu_up.yml`(e1ap/f1u 포함)은 CU-UP **단독** 실행용으로 두고, gNB와 함께 쓸 때는 `cu_up_ngu_only.yml`만 사용.

---

## 7. iptables 규칙 (중요: 소스 IP)

- **규칙의 `-s`(소스 IP)는 “N3 패킷이 실제로 오는 출발지 IP”와 같아야 함.**
- 처음에 10.53.1.2로 했는데 **pkts=0**이면, 로그로 **실제 소스 IP**를 확인한 뒤 그 IP로 규칙을 넣어야 함.

### 7.1 실제 소스 IP 확인

```bash
grep "\[IPTABLES-DSCP\] Extracted outer" /tmp/gnb.log | tail -3
```

- 여기서 **src=** 뒤에 나오는 IP가 **실제 N3 소스(UPF)** 이다.  
  예: `src=192.168.0.3:2152` → `-s 192.168.0.3` 사용.

### 7.2 규칙 추가 (소스 IP를 실제 값으로)

```bash
# 예: 실제 소스가 192.168.0.3 인 경우
sudo iptables -t mangle -A PREROUTING -s 192.168.0.3 -p udp --dport 2152 -j DSCP --set-dscp 32
```

- 10.53.1.2를 쓰는 환경이면:

```bash
sudo iptables -t mangle -A PREROUTING -s 10.53.1.2 -p udp --dport 2152 -j DSCP --set-dscp 32
```

### 7.3 규칙 확인 / 삭제

```bash
# 확인 (pkts > 0 이면 패킷이 규칙에 걸린 것)
sudo iptables -t mangle -L PREROUTING -n -v

# 규칙 삭제 (예: 192.168.0.3)
sudo iptables -t mangle -D PREROUTING -s 192.168.0.3 -p udp --dport 2152 -j DSCP --set-dscp 32
```

---

## 8. 동작 확인

### 8.1 트래픽 발생

- UE 접속 후 호스트에서 UE IP로 ping:

```bash
ping -c 5 10.45.0.2
```

(UE IP가 다르면 해당 IP로.)

### 8.2 로그로 단계별 확인

```bash
# 1) outer ToS 수신 (UDP 게이트웨이)
grep "\[IPTABLES-DSCP\] Extracted outer" /tmp/gnb.log | tail -3
# 기대: ToS=0x80 (DSCP=32), src=<실제 UPF IP>:2152

# 2) inner ToS 복사 (GTP-U)
grep "\[IPTABLES-DSCP\] DSCP copied" /tmp/gnb.log | tail -2
# 기대: outer ToS=0x80 -> inner ToS=0x00->0x80 (DSCP=32)

# 3) SDAP DSCP 추출
grep "\[STEP1-SDAP\].*DSCP" /tmp/gnb.log | tail -2
# 기대: DSCP 추출 성공 ... DSCP=32
```

### 8.3 iptables 매칭 확인

```bash
sudo iptables -t mangle -L PREROUTING -n -v
```

- 해당 DSCP 규칙의 **pkts**가 0보다 크면 N3 패킷에 규칙이 적용된 것.

---

## 9. 트러블슈팅

| 증상 | 확인/조치 |
|------|------------|
| **ToS=0x00, src=127.0.0.x** | N3가 아직 로컬호스트. UPF gtpu 주소·gNB ngu bind_addr를 실제 IP로 맞추고, 재시작 후 UE 재접속. |
| **ToS=0x00, src=실제 IP** | iptables 규칙이 없거나 `-s`가 다름. 로그의 `src=`와 동일한 IP로 규칙 추가. |
| **iptables pkts=0** | N3 소스가 규칙의 `-s`와 다름. `grep "Extracted outer" gnb.log`로 실제 `src=` 확인 후 `-s` 수정. |
| **INI was not able to parse cu_up.e1ap.++** | gNB에 e1ap/f1u가 있는 cu_up 설정을 줌. **cu_up_ngu_only.yml** 처럼 ngu만 넣은 파일 사용. |
| **DSCP not supported** | 커널에 `xt_DSCP` 없음. `lsmod \| grep xt_DSCP`, `modprobe xt_DSCP` (실제 리눅스면 보통 있음). |

---

## 10. 참고: 같은 PC에서 돌릴 때

- Open5GS(UPF)와 gNB를 **같은 PC**에서 돌리면, N3 패킷의 **소스 IP**는 그 PC의 실제 인터페이스 IP(예: 192.168.0.3)가 됨.
- 따라서 iptables의 **`-s 192.168.0.3`** 은 “우리 PC(UPF)에서 나온 패킷”을 의미함.  
  같은 머신이어도 N3가 실제 인터페이스로 나갔다 들어오므로 PREROUTING에 걸리고, DSCP가 적용되는 것이 맞음.

---

## DSCP가 안 들어갈 때

- **N3 전체는 되는데, UE별(u32) DL에서 DSCP가 안 들어가는 경우**
  - `xt_u32` 모듈 필요: `sudo modprobe xt_u32` 후 스크립트 재실행.
  - 확인: `lsmod | grep xt_u32` 로 로드 여부 확인.
- **규칙은 있는데 패킷 카운트가 0인 경우**
  - N3 소스가 실제 IP인지 확인: `grep "Extracted outer" gnb.log` 에서 `src=` 값이 `UPF_IP`와 일치하는지.
  - PREROUTING만 쓰면 로컬 수신 트래픽에 안 걸릴 수 있음 → 스크립트는 PREROUTING + INPUT 둘 다 사용.
- **u32 UE별 규칙**  
  GTP-U 내부 IP 목적지(outer IP 기준 52~55바이트)로 UE 구분. 오프셋 52 = IP(20) + UDP(8) + GTP-U(8) + inner IP dst(16).

---

## iptables는 적용되는데 SDAP 로그는 전부 DSCP=0인 경우 (같은 호스트 로컬 전달)

**증상:** `iptables -t mangle -L PREROUTING -n -v` 에서 u32 규칙 pkts는 오르는데, gnb 로그의 `[STEP1-SDAP] DSCP 추출 성공` 은 전부 DSCP=0이고 `grep "DSCP=32"` / `grep "DSCP=14"` 가 안 나옴.

**원인:** UPF와 gNB가 **같은 머신**에서 동작할 때, 커널이 N3 패킷을 **로컬 전달**(한 프로세스→다른 프로세스)로 넘기면 **수신 측 INPUT 체인을 타지 않습니다**. 그래서 iptables로 잡히는 패킷(다른 경로로 들어온 것)만 ToS가 바뀌고, gNB 소켓으로 오는 대부분 패킷은 **수정 전 ToS(0)** 만 `IP_RECVTOS` 로 전달됩니다.  
→ UDP 게이트웨이는 outer ToS=0만 받고, GTP-U 터널이 inner ToS를 0으로 두고, SDAP는 계속 DSCP=0만 보게 됨.

**확인 방법:**
- Phase 2 구간에 gnb 로그에서 outer ToS를 확인:
  - `grep "\[IPTABLES-DSCP\] Extracted outer" /tmp/gnb.log | grep "ToS=0x80"`
  - 한 줄도 안 나오면 → gNB 소켓에는 수정된 ToS가 **한 번도** 안 들어온 것 (로컬 전달로 INPUT 미경유).
- SDAP는 **inner IP ToS**에서 DSCP를 읽음. inner는 GTP-U 터널이 outer ToS를 복사한 값이므로, outer가 0이면 SDAP는 항상 0.

**해결 방향:** 아래 섹션 **"같은 PC에서 N3가 한 번 실제 인터페이스를 타게 하기"** 에서 dummy 두 개(10.53.1.2 / 10.53.1.3) 만들고 UPF·gNB·iptables 설정하면 UE별 DSCP가 SDAP까지 전달된다.

1. **N3를 반드시 “와이어”로 한 번 나갔다 들어오게 하기**  
   - gNB N3 바인드 주소와 UPF N3 주소를 **서로 다른 IP**로 두고, 실제로 그 구간으로 패킷이 나갔다 들어오게 구성 (예: dummy/브릿지, 또는 다른 랜 세그먼트).  
   - 그러면 수신 시 **INPUT**을 타서 iptables가 적용되고, 같은 ToS가 소켓(`IP_RECVTOS`) → GTP-U → SDAP까지 전달됨.
2. **UPF와 gNB를 서로 다른 머신/VM에서 실행**  
   - N3가 물리/가상 네트워크를 한 번 지나가게 하면, gNB 측에서 모두 INPUT을 타서 위와 같이 동작.
3. (참고) **로그의 UE 인덱스**  
   - `[STEP1-SDAP] ... UE=1 ...` 의 UE=1이 우리 스크립트의 “UE1(10.45.0.2)”가 아닐 수 있음.  
   - Phase 2에서 `grep "STEP1-SDAP.*DSCP=32"` 가 안 나온다면, 위 1·2처럼 N3 경로를 바꿔서 “수신 시 INPUT 경유”가 되도록 하는 것이 우선.

---

## 같은 PC에서 N3가 한 번 실제 인터페이스를 타게 하기 (UE별 DSCP 적용)

같은 PC에서 UPF와 gNB를 돌리면서 UE별로 다른 DSCP를 주려면, **N3 패킷이 수신 측 INPUT을 반드시 타게** 해야 한다. 한 인터페이스에 두 IP를 두면 커널이 로컬 전달로 넘겨 INPUT을 안 탈 수 있으므로, **UPF용 IP와 gNB용 IP를 서로 다른 인터페이스**에 둔다.

### 1단계: dummy 인터페이스 두 개 만들기

UPF N3용 **10.53.1.2**, gNB N3용 **10.53.1.3** 를 **각각 다른 dummy**에 둔다.

```bash
sudo ./scripts/setup_n3_dummy.sh
# 또는 수동:
sudo ip link add name n3upf type dummy
sudo ip link add name n3gnb type dummy
sudo ip addr add 10.53.1.2/24 dev n3upf
sudo ip addr add 10.53.1.3/24 dev n3gnb
sudo ip link set n3upf up
sudo ip link set n3gnb up
```

`ip addr` 로 n3upf에만 10.53.1.2, n3gnb에만 10.53.1.3 이 보이면 된다.

### 2단계: Open5GS UPF 설정

UPF가 N3를 **10.53.1.2**에서 나가게 한다. **파일:** Open5GS UPF 설정 (예: `install/etc/open5gs/upf.yaml`)

```yaml
upf:
  gtpu:
    server:
      - address: 10.53.1.2
```

저장 후 **UPF 재시작**.

### 3단계: gNB N3 바인드

gNB는 N3를 **10.53.1.3**에서 받게 한다. **파일:** `configs/cu_up_ngu_only.yml`

```yaml
cu_up:
  ngu:
    socket:
      -
        bind_addr: 10.53.1.3
```

실행: `./gnb -c gnb_zmq.yaml -c configs/cu_up_ngu_only.yml`

### 4단계: iptables 및 테스트 스크립트

N3 소스가 10.53.1.2 이므로:

```bash
sudo iptables -t mangle -A PREROUTING -s 10.53.1.2 -p udp --dport 2152 -j DSCP --set-dscp 32
sudo iptables -t mangle -A INPUT -s 10.53.1.2 -p udp --dport 2152 -j DSCP --set-dscp 32
```

UE별(u32) 스크립트 사용 시: `export UPF_IP=10.53.1.2` 후 스크립트 실행.

### 5단계: 재시작 및 UE 재접속

UPF·gNB 재시작 후, UE 한 번 끊었다가 다시 접속해서 PDU 세션이 새 N3(10.53.1.2→10.53.1.3)로 잡히게 한다.

### 6단계: 확인

```bash
grep "\[IPTABLES-DSCP\] Extracted outer" /tmp/gnb.log | grep -E "ToS=0x80|ToS=0x38"
grep "\[STEP1-SDAP\].*DSCP 추출 성공" /tmp/gnb.log | grep -E "DSCP=32|DSCP=14"
```

ToS=0x80, STEP1-SDAP에서 DSCP=32/14 가 보이면 N3가 실제 인터페이스를 타고 INPUT까지 들어온 것이다.  
재부팅 시 dummy는 사라지므로, 테스트 전에 `sudo ./scripts/setup_n3_dummy.sh` 를 다시 실행하면 된다.

---

## 같은 PC에서 UE별 DSCP 시도 요약 (왜 안 됐는지)

**목표:** UPF와 gNB를 **같은 PC**에서 돌리면서, N3 DL에 대해 **UE별로 다른 DSCP**(UE1만 32→14, UE2/3=0)를 주고 SDAP/스케줄러까지 전달되게 하기.

### 1. 시도한 환경

- Open5GS(UPF 포함)·gNB·UE 모두 **한 Linux 머신**에서 실행.
- N3: 처음에는 **192.168.0.3** (실제 인터페이스 한 개)으로 동작.

### 2. 진행한 단계 (순서대로)

| 단계 | 한 일 | 결과/확인 |
|------|--------|-----------|
| 1 | iptables로 `-s 192.168.0.3` 한 규칙 또는 u32로 UE별 규칙 추가 | PREROUTING/INPUT에 규칙 추가됨, **pkts는 증가** (규칙은 매칭됨). |
| 2 | gNB 로그 확인: `grep "\[IPTABLES-DSCP\] Extracted outer" gnb.log` | **ToS=0x00, src=192.168.0.3** 만 보임. SDAP는 전부 DSCP=0. |
| 3 | 원인 정리 | 같은 호스트에서 UPF→gNB N3가 **로컬 전달**로 가서, 수신 측 **INPUT을 타지 않거나** 소켓에 **원래 ToS(0)** 만 전달됨. iptables는 걸리지만 gNB 프로세스가 받는 패킷에는 수정된 ToS가 반영되지 않음. |
| 4 | dummy 인터페이스 **두 개** 생성 (n3upf=10.53.1.2, n3gnb=10.53.1.3) | `ip addr` 로 10.53.1.2 / 10.53.1.3 각각 확인됨. |
| 5 | UPF `upf.yaml`: `gtpu.server[].address: 10.53.1.2` | 적용 후 UPF 재시작. `ss -ulnp \| grep 2152` → **open5gs-upfd가 10.53.1.2:2152** 에 바인드됨. |
| 6 | gNB `cu_up_ngu_only.yml`: `bind_addr: 10.53.1.3` | gNB를 `-c cu_up_ngu_only.yml` 로 실행. `ss -ulnp \| grep 2152` → **gnb가 10.53.1.3:2152** 에 바인드됨. |
| 7 | 라우트 추가: `ip route add 10.53.1.3/32 dev n3upf src 10.53.1.2` | "File exists" 또는 추가됨. `ip route get 10.53.1.3` → **local 10.53.1.3 dev lo table local** 로 나옴 (10.53.1.3이 같은 머신 로컬 주소라 로컬 전달로 처리됨). |
| 8 | `export UPF_IP=10.53.1.2` 후 iptables 규칙 추가 또는 스크립트 실행 | `-s 10.53.1.2` 규칙은 **pkts=0** (해당 소스로 오는 패킷 없음). |
| 9 | UE 전부 끊었다가 다시 접속 (PDU 세션 재설립) | 재접속 후에도 동일. |
| 10 | ping 10.45.0.2 등으로 트래픽 발생 후 로그 재확인 | 여전히 **src=192.168.0.3, ToS=0x00**. |

### 3. 왜 안 됐는지 (정리)

- **같은 PC에 10.53.1.2(UPF)와 10.53.1.3(gNB)이 둘 다 있으면**, 커널은 10.53.1.3을 **로컬 주소**로 인식한다.
- UPF가 10.53.1.3으로 N3를 보낼 때, 커널이 **로컬 전달**로 넘기고, 이 경로에서는:
  - **출발지(src)** 가 10.53.1.2가 아니라 **다른 인터페이스 IP(예: 192.168.0.3)** 로 나가거나,
  - 수신 측에서 **INPUT 체인을 타지 않거나**, 타더라도 **소켓(IP_RECVTOS)에는 원래 ToS(0)** 만 넘어간다.
- 그래서 **dummy 두 개 + 라우트**만으로는 “한 번 실제 인터페이스로 나갔다 들어오는” 경로가 되지 않고, 로그에는 계속 **src=192.168.0.3, ToS=0x00** 만 보인다.

### 4. 같은 PC에서 성공하려면 (선택지)

| 방법 | 설명 |
|------|------|
| **UPF를 다른 머신/VM에서 실행** | N3가 네트워크로 한 번 나갔다 들어오므로, gNB 쪽에서 수신 시 INPUT을 타고, 소켓에 수정된 ToS가 전달됨. `UPF_IP`만 UPF 머신 IP로 맞추면 됨. |
| **gNB를 네트워크 네임스페이스 + veth로 분리** | 10.53.1.3을 netns 안에 두고, 호스트에는 10.53.1.2만 두면, 10.53.1.3으로 가는 패킷이 veth를 타고 netns로 들어가서 “수신” 경로를 탈 수 있음. gNB를 netns 안에서 실행해야 하며, RU/AMF 등 연결 설정이 필요함. |

### 5. 참고: 스크립트 기본값

- `iperf3_dynamic_dscp_test.sh` 의 **UPF_IP** 기본값은 `192.168.0.3` 이다.
- dummy(10.53.1.2/10.53.1.3)를 쓰거나, UPF를 다른 머신에 두었으면 **반드시** `export UPF_IP=<실제_N3_소스_IP>` 로 맞춘 뒤 스크립트를 실행해야 iptables 규칙이 해당 소스에 걸린다.

---

## 요약

1. **N3를 127.0.0.x가 아닌 실제 IP로** 쓰게 설정 (UPF `upf.yaml` gtpu, gNB `cu_up_ngu_only.yml` ngu).
2. **실제 N3 소스 IP**를 로그(`src=`)로 확인한 뒤, 그 IP로  
   `iptables -t mangle -A PREROUTING -s <그IP> -p udp --dport 2152 -j DSCP --set-dscp 32`  
   적용.
3. gNB는 **cu_up_ngu_only.yml** 로만 cu_up을 넘겨서 파싱 오류 방지.
4. 주소 변경 후에는 **Open5GS + gNB 재시작** 후 **UE 재접속**해서 PDU 세션을 새 N3로 잡게 함.

이 순서대로 하면 outer DSCP가 inner로 복사되고 SDAP까지 전달됩니다.
