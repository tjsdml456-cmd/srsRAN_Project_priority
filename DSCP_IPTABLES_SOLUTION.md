# iptables를 사용한 DSCP 변경 및 SDAP 추출 문제 해결 방법

## 📋 목차

1. [문제 상황](#문제-상황)
2. [구현 완료된 코드 변경 사항](#구현-완료된-코드-변경-사항)
3. [현재 문제점 및 원인](#현재-문제점-및-원인)
4. [해결 방법](#해결-방법)
5. [테스트 방법](#테스트-방법)

---

## 문제 상황

srsRAN에서 iptables를 사용하여 외부 IP 패킷의 DSCP 값을 변경했을 때, SDAP 레이어에서 DSCP를 추출하지 못하는 문제가 발생했습니다.

### 패킷 구조 및 흐름

**패킷 구조:**
```
외부 IP 헤더 (iptables가 여기서 DSCP 변경)
  └─ UDP 헤더 (GTP-U 터널, 포트 2152)
      └─ GTP-U 헤더
          └─ 내부 IP 헤더 (iperf3 TCP 트래픽)
              └─ TCP 헤더
                  └─ 데이터
```

**패킷 흐름:**
```
외부 서버 (iperf3 - TCP 연결)
    ↓ (TCP 트래픽이 내부 IP 패킷으로 캡슐화)
    ↓ (내부 IP 패킷이 GTP-U로 캡슐화)
    ↓ (GTP-U가 UDP로 캡슐화)
    ↓ (외부 IP 패킷, DSCP=32로 iptables가 변경)
UDP 네트워크 게이트웨이 (UDP 소켓)
    ↓ (GTP-U 패킷 수신, 외부 IP 헤더는 이미 제거됨)
GTP-U 터널 (gtpu_tunnel_ngu_rx_impl)
    ↓ (GTP-U 헤더 제거, 내부 IP 패킷 추출)
SDAP (sdap_entity_tx_impl)
    ↓ (내부 IP 패킷에서 DSCP 추출 - iperf3 TCP 트래픽의 ToS)
dscp_qos_mapper
    ↓
Scheduler (scheduler_time_qos)
```

### 문제의 핵심

- **iperf3는 TCP 연결을 사용함**
  - iperf3는 TCP 소켓을 사용하여 데이터를 전송합니다.
  - TCP 트래픽은 내부 IP 패킷으로 캡슐화됩니다.
  
- **GTP-U 터널은 UDP를 사용함**
  - 내부 IP 패킷(iperf3 TCP 트래픽)이 GTP-U 헤더로 캡슐화됩니다.
  - GTP-U 패킷은 UDP로 캡슐화되어 외부 IP 패킷으로 전송됩니다.
  
- **iptables는 외부 IP 패킷(GTP-U UDP 포함)에 적용됨**
  - iptables는 네트워크 레벨에서 작동하며, UDP 패킷(GTP-U 포함)을 포함하는 외부 IP 패킷의 DSCP를 변경합니다.
  - 하지만 이 변경은 외부 IP 헤더에만 적용되며, 내부 IP 패킷(iperf3 TCP 트래픽)의 ToS에는 영향을 주지 않습니다.
  
- **SDAP는 GTP-U 터널 내부의 IP 패킷(추출된 IP)을 처리함**
  - GTP-U 터널에서 `gtpu_extract_msg()`를 통해 GTP-U 헤더를 제거하고 내부 IP 패킷을 추출합니다.
  - SDAP는 이 추출된 내부 IP 패킷(iperf3 TCP 트래픽)의 ToS 필드에서 DSCP를 추출합니다.
  
- **결과: 외부 IP 패킷의 DSCP 변경이 내부 IP 패킷에 반영되지 않음**
  - iptables로 외부 IP 패킷의 DSCP를 변경해도, 내부 IP 패킷(iperf3 TCP 트래픽)의 ToS는 원래 값 그대로입니다.
  - 따라서 SDAP에서 추출하는 DSCP 값은 변경되지 않습니다.

---

## 구현 완료된 코드 변경 사항

### ✅ 완료된 작업

iptables로 변경한 외부 IP 패킷의 DSCP를 내부 IP 패킷의 ToS로 복사하는 로직이 **모두 구현 완료**되었습니다.

### 📁 수정된 파일 목록 (총 11개)

#### 1. UDP 네트워크 게이트웨이 (`lib/gateways/udp_network_gateway_impl.cpp`)

**변경 사항:**
- `IP_PKTINFO` 및 `IP_RECVTOS` 소켓 옵션 활성화
- `recvmmsg()` 호출 시 `msghdr.msg_control`에서 `IP_TOS` 제어 메시지 추출
- 외부 IP 패킷의 ToS 값을 `outer_tos`로 추출
- `data_notifier.on_new_pdu()` 호출 시 `outer_tos` 전달
- 소스 IP 주소 및 포트 정보 로깅 추가 (진단용)

**주요 코드:**
```cpp
// 소켓 옵션 설정
setsockopt(sock_fd.value(), IPPROTO_IP, IP_PKTINFO, &val, sizeof(val));
setsockopt(sock_fd.value(), IPPROTO_IP, IP_RECVTOS, &val, sizeof(val));

// ToS 추출
for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg)) {
  if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TOS) {
    outer_tos = *(uint8_t*)CMSG_DATA(cmsg);
    // outer_tos를 GTP-U 터널로 전달
  }
}
```

#### 2. 네트워크 게이트웨이 인터페이스 (`include/srsran/gateways/network_gateway.h`)

**변경 사항:**
- `network_gateway_data_notifier_with_src_addr` 인터페이스에 `outer_tos` 파라미터 추가
- `on_new_pdu()` 함수 시그니처에 `std::optional<uint8_t> outer_tos = {}` 추가

#### 3. GTP-U Demux (`include/srsran/gtpu/gtpu_demux.h`, `lib/gtpu/gtpu_demux_impl.cpp`, `lib/gtpu/gtpu_demux_impl.h`)

**변경 사항:**
- `gtpu_demux_pdu_ctx_t` 구조체에 `outer_tos` 필드 추가
- `gtpu_demux_rx_upper_layer_interface::handle_pdu()` 시그니처에 `outer_tos` 파라미터 추가
- `handle_pdu()` 및 `handle_pdu_impl()` 함수에서 `outer_tos` 전달

#### 4. GTP-U 터널 Base RX (`lib/gtpu/gtpu_tunnel_base_rx.h`)

**변경 사항:**
- `gtpu_tunnel_base_rx::handle_pdu()` 가상 함수 시그니처에 `outer_tos` 파라미터 추가

#### 5. GTP-U 터널 NGU RX (`lib/gtpu/gtpu_tunnel_ngu_rx_impl.h`)

**변경 사항:**
- `handle_pdu()` 함수에서 `outer_tos` 파라미터 받음
- `gtpu_extract_msg()`로 내부 IP 패킷 추출 후, 내부 IP 패킷의 ToS 필드를 외부 IP 패킷의 ToS로 복사
- ECN 비트는 유지하고 DSCP만 복사
- 로깅 추가 (DSCP 복사 성공/실패)

**주요 코드:**
```cpp
void handle_pdu(gtpu_dissected_pdu&& pdu, const sockaddr_storage& src_addr, std::optional<uint8_t> outer_tos = {}) final
{
  byte_buffer rx_sdu = gtpu_extract_msg(std::move(pdu));
  
  // 내부 IP 패킷의 ToS 필드를 외부 IP 패킷의 ToS로 복사
  if (outer_tos.has_value() && rx_sdu.length() >= 20) {
    uint8_t version_ihl = rx_sdu[0];
    if ((version_ihl >> 4) == 4) { // IPv4
      uint8_t inner_tos = rx_sdu[1];
      uint8_t ecn = inner_tos & 0x03; // ECN 유지
      uint8_t outer_dscp = (outer_tos.value() >> 2) & 0x3F; // 외부 DSCP 추출
      uint8_t new_tos = (outer_dscp << 2) | ecn; // 내부 ToS를 외부 DSCP로 설정
      rx_sdu[1] = new_tos;
    }
  }
}
```

#### 6. GTP-U 터널 NRU RX (`lib/gtpu/gtpu_tunnel_nru_rx_impl.h`)

**변경 사항:**
- NGU와 동일한 로직 적용 (NRU 터널용)

#### 7. GTP-U Gateway (`lib/gtpu/gtpu_gateway.cpp`)

**변경 사항:**
- `on_new_pdu()` 함수에서 `outer_tos` 받아서 `gtpu_demux`로 전달

#### 8. CU-UP Adapters (`lib/cu_up/adapters/gw_adapters.h`)

**변경 사항:**
- `network_gateway_data_gtpu_demux_adapter::on_new_pdu()` 시그니처에 `outer_tos` 파라미터 추가 및 전달

#### 9. F1U Split Connector (CU-UP, DU)

**변경 사항:**
- `lib/f1u/cu_up/split_connector/f1u_split_connector.cpp`
- `lib/f1u/du/split_connector/f1u_split_connector.h`
- `network_gateway_data_gtpu_demux_adapter::on_new_pdu()` 시그니처에 `outer_tos` 파라미터 추가 및 전달

#### 10. GTP-U Echo RX (`lib/gtpu/gtpu_echo_rx_impl.h`)

**변경 사항:**
- `handle_pdu()` 함수 시그니처에 `outer_tos` 파라미터 추가 (호환성 유지)

### 🔄 데이터 흐름

```
1. UDP Gateway (udp_network_gateway_impl.cpp)
   - recvmmsg()로 패킷 수신
   - IP_TOS 제어 메시지에서 outer_tos 추출
   - outer_tos를 GTP-U Gateway로 전달

2. GTP-U Gateway (gtpu_gateway.cpp)
   - outer_tos를 GTP-U Demux로 전달

3. GTP-U Demux (gtpu_demux_impl.cpp)
   - outer_tos를 GTP-U Tunnel로 전달

4. GTP-U Tunnel NGU RX (gtpu_tunnel_ngu_rx_impl.h)
   - gtpu_extract_msg()로 내부 IP 패킷 추출
   - 내부 IP 패킷의 ToS 필드를 outer_tos로 복사
   - ECN 비트는 유지

5. SDAP (sdap_entity_tx_impl.h)
   - 내부 IP 패킷의 ToS에서 DSCP 추출
   - dscp_qos_mapper에 등록

6. Scheduler (scheduler_time_qos.cpp)
   - DSCP 기반 5QI 매핑
   - 우선순위 계산 및 자원 할당
```

---

## 현재 문제점 및 원인

### ❌ 문제 1: WSL2에서 iptables DSCP 모듈이 없음

**증상:**
```bash
$ sudo iptables -t mangle -A INPUT -p udp --dport 2152 -j DSCP --set-dscp 32
Warning: Extension DSCP revision 0 not supported, missing kernel module?
iptables: No chain/target/match by that name.
```

**원인:**
- WSL2 커널 (`5.15.167.4-microsoft-standard-WSL2`)에 `xt_DSCP` 모듈이 포함되어 있지 않음
- `modprobe xt_DSCP` 실행 시 모듈을 찾을 수 없음

**확인 방법:**
```bash
# 모듈 확인
lsmod | grep xt_DSCP
# 결과: 없음

# 모듈 로드 시도
sudo modprobe xt_DSCP
# 결과: modprobe: FATAL: Module xt_DSCP not found
```

### ❌ 문제 2: 로컬호스트 트래픽 (127.0.0.7)

**증상:**
- 모든 패킷이 `127.0.0.7:2152`에서 옴 (로컬호스트)
- 모든 패킷의 ToS가 `0x00` (DSCP=0)

**로그 예시:**
```
[IPTABLES-DSCP] Extracted outer IP ToS=0x00 (DSCP=0) from UDP packet src=127.0.0.7:2152 len=76 type=로컬호스트
```

**원인:**
- UPF가 `127.0.0.7`에서 실행되어 로컬호스트 통신 사용
- 로컬호스트 트래픽은 iptables `PREROUTING` 체인을 거치지 않음
- `INPUT` 체인만 거치지만, 로컬호스트 패킷은 커널 내부에서 처리되어 iptables 규칙이 적용되지 않을 수 있음

**확인 방법:**
```bash
# 소스 IP 확인
grep "\[IPTABLES-DSCP\].*Extracted outer" gnb.log | grep "src=" | head -5
# 결과: 모두 src=127.0.0.7:2152

# iptables 규칙 확인
sudo iptables -t mangle -L PREROUTING -n -v | grep "2152"
# 결과: 규칙은 있지만 패킷 카운터가 0 (pkts=0)
```

### ❌ 문제 3: iptables-legacy로 전환해도 동일한 문제

**시도한 방법:**
```bash
sudo update-alternatives --set iptables /usr/sbin/iptables-legacy
```

**결과:**
- iptables-legacy로 전환 성공
- 하지만 여전히 DSCP 모듈이 없어서 규칙 추가 실패

---

## 해결 방법

### ✅ 옵션 1: 실제 네트워크 인터페이스 사용 (권장)

로컬호스트 대신 실제 네트워크 IP를 사용하여 iptables 규칙이 적용되도록 합니다.

#### 1단계: CU-UP N3 인터페이스 설정 변경

**파일:** `configs/cu_up.yml`

**변경 전:**
```yaml
cu_up:
  ngu:
    socket:
      -
        bind_addr: 127.0.0.1  # 로컬호스트
```

**변경 후:**
```yaml
cu_up:
  ngu:
    socket:
      -
        bind_addr: 10.53.1.3  # 실제 네트워크 IP (gNB IP)
        # 또는 "auto"로 설정하면 자동으로 네트워크 인터페이스 IP를 찾음
```

**또는 Docker Compose에서:**
```yaml
gnb_compose_config.yml:
  content: |
    cu_up:
      ngu:
        socket:
          -
            bind_addr: ${GNB_IP:-10.53.1.3}  # gnb 컨테이너의 IP
```

#### 2단계: iptables 규칙 적용

**실제 네트워크 인터페이스에 iptables 규칙 적용:**

```bash
# 방법 1: 소스 IP로 필터링 (UPF에서 오는 패킷)
sudo iptables -t mangle -A PREROUTING -s 10.53.1.2 -p udp --dport 2152 -j DSCP --set-dscp 32

# 방법 2: 네트워크 인터페이스로 필터링
# 먼저 네트워크 인터페이스 확인
ip addr show | grep "10.53.1"
# 예: eth0 또는 docker0

sudo iptables -t mangle -A PREROUTING -i eth0 -p udp --dport 2152 -j DSCP --set-dscp 32
```

#### 3단계: 테스트 스크립트 수정

**파일:** `iperf3_dynamic_dscp_test.sh`

`change_dscp_via_iptables()` 함수 수정:

```bash
change_dscp_via_iptables() {
    local ue_index=$1
    local dscp_value=$2
    
    local upf_ip="10.53.1.2"  # UPF IP
    local gnb_ip="10.53.1.3"  # gNB IP
    
    # DL 트래픽: UPF에서 gNB로 오는 패킷
    # PREROUTING 체인 - 소스 IP가 UPF인 패킷에 DSCP 설정
    sudo iptables -t mangle -A PREROUTING -s $upf_ip -p udp --dport 2152 -j DSCP --set-dscp $dscp_value
    
    # 또는 네트워크 인터페이스로 필터링
    # sudo iptables -t mangle -A PREROUTING -i <인터페이스> -p udp --dport 2152 -j DSCP --set-dscp $dscp_value
}
```

#### 장점
- ✅ 코드 수정 없이 설정만 변경
- ✅ Docker 네트워크 활용 가능
- ✅ iptables 규칙이 정상적으로 적용됨
- ✅ 네이티브 리눅스에서 확실하게 작동

#### 단점
- ⚠️ WSL2에서는 여전히 DSCP 모듈이 없을 수 있음

---

### ✅ 옵션 2: 네이티브 리눅스 환경 사용

WSL2 대신 네이티브 리눅스 환경에서 테스트합니다.

#### 장점
- ✅ iptables DSCP 모듈이 일반적으로 포함되어 있음
- ✅ 로컬호스트 트래픽도 INPUT 체인에서 처리 가능할 수 있음
- ✅ 옵션 1과 함께 사용하면 확실하게 작동

#### 단점
- ⚠️ WSL2 환경을 포기해야 함

---

### ✅ 옵션 3: WSL2 커널 업그레이드 (제한적)

Microsoft 최신 WSL2 커널로 업데이트합니다.

**Windows PowerShell에서:**
```powershell
wsl --update
wsl --shutdown
# WSL 재시작
```

**확인:**
```bash
uname -r
# 최신 커널 버전 확인

lsmod | grep xt_DSCP
# 모듈 확인
```

**주의:**
- 최신 커널에도 `xt_DSCP` 모듈이 포함되어 있다는 보장은 없음
- 커스텀 커널 빌드는 복잡함

---

## 테스트 방법

### 1. 현재 상태 확인

```bash
# 1. 소스 IP 확인
grep "\[IPTABLES-DSCP\].*Extracted outer" gnb.log | grep "src=" | head -5

# 2. ToS 값 확인
grep "\[IPTABLES-DSCP\].*Extracted outer" gnb.log | grep -E "ToS=0x[0-9a-f]{2}" | head -10

# 3. iptables 규칙 확인
sudo iptables -t mangle -L PREROUTING -n -v | grep "2152"
sudo iptables -t mangle -L INPUT -n -v | grep "2152"

# 4. 패킷 카운터 확인 (규칙이 매칭되는지)
# pkts > 0 이면 규칙이 적용되고 있음
```

### 2. 옵션 1 적용 후 테스트

```bash
# 1. cu_up.yml 수정 (bind_addr를 실제 IP로 변경)

# 2. gNB 재시작

# 3. iptables 규칙 추가
sudo iptables -t mangle -A PREROUTING -s 10.53.1.2 -p udp --dport 2152 -j DSCP --set-dscp 32

# 4. 테스트 스크립트 실행
./iperf3_dynamic_dscp_test.sh

# 5. 로그 확인
grep "\[IPTABLES-DSCP\].*Extracted outer" gnb.log | grep -E "ToS=0x80|DSCP=32"
grep "\[STEP1-SDAP\].*DSCP 추출" gnb.log | grep -E "DSCP=32|DSCP=14"
```

### 3. 성공 확인

**로그에서 확인할 사항:**
1. **UDP Gateway 로그:**
   ```
   [IPTABLES-DSCP] Extracted outer IP ToS=0x80 (DSCP=32) from UDP packet src=10.53.1.2:2152 type=외부
   ```

2. **GTP-U Tunnel 로그:**
   ```
   [IPTABLES-DSCP] DSCP copied: outer ToS=0x80 (DSCP=32) -> inner ToS=0x00->0x80 (DSCP=32)
   ```

3. **SDAP 로그:**
   ```
   [STEP1-SDAP] DSCP 추출 성공 - UE=0 PSI=1 QFI=1 DRB=1 DSCP=32 (0x80)
   ```

4. **Scheduler 로그:**
   ```
   [STEP6-DSCP-LOOKUP] UE0 LCID4 DSCP=32 조회 성공
   [STEP7-SCHED] Priority 계산: UE0 LCID4 effective_5qi=69 priority=5
   ```

---

## 결론

### ✅ 완료된 작업
- **코드 수정 완료**: outer ToS 추출 → inner ToS로 복사하는 로직 모두 구현 완료
- **데이터 흐름 완성**: UDP Gateway → GTP-U Tunnel → SDAP → Scheduler까지 전체 흐름 구현

### ❌ 남은 문제
- **WSL2 환경 제한**: iptables DSCP 모듈이 없음
- **로컬호스트 트래픽**: iptables 규칙이 적용되지 않음

### 🎯 권장 해결 방법
1. **옵션 1 (실제 네트워크 인터페이스 사용)**: 가장 현실적이고 확실한 방법
2. **옵션 2 (네이티브 리눅스)**: 옵션 1과 함께 사용하면 확실하게 작동
3. **옵션 3 (WSL2 커널 업그레이드)**: 보장되지 않지만 시도해볼 수 있음

### 📝 다음 단계
1. `configs/cu_up.yml`에서 `ngu.socket.bind_addr`를 실제 네트워크 IP로 변경
2. iptables 규칙을 실제 네트워크 인터페이스에 적용
3. 테스트 스크립트 실행 및 로그 확인
4. 네이티브 리눅스 환경에서 최종 테스트 (WSL2 제한 회피)

---

## 참고 자료

- **iptables 체인 설명:**
  - `PREROUTING`: 라우팅 전, 외부에서 들어오는 패킷
  - `INPUT`: 로컬 호스트로 들어오는 패킷
  - `OUTPUT`: 로컬에서 생성된 패킷
  - `POSTROUTING`: 라우팅 후, 나가는 패킷
  - `FORWARD`: 라우팅되는 패킷

- **로컬호스트 트래픽:**
  - `127.0.0.x` 또는 `::1`에서 오는 패킷
  - `PREROUTING` 체인을 거치지 않음
  - `INPUT` 체인만 거치지만, 커널 내부 처리로 iptables 규칙이 적용되지 않을 수 있음

- **DSCP 모듈:**
  - `xt_DSCP`: iptables DSCP 타겟을 제공하는 Netfilter 확장 모듈
  - 일반 리눅스 커널에는 포함되어 있지만, WSL2 커널에는 포함되지 않을 수 있음
