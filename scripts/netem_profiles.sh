#!/usr/bin/env bash
# netem_profiles.sh -- apply or clear named tc netem impairment profiles on
# a network interface, for the latency sweep described in
# docs/NETWORK_TESTING_AND_LATENCY.md.
#
# Two modes:
#   1. Direct interface mode (simplest, good enough for most testing):
#        sudo ./netem_profiles.sh apply <iface> <profile>
#        sudo ./netem_profiles.sh clear <iface>
#      Applies netem directly to an interface you already have (e.g. a veth
#      endpoint, or even the primary interface if testing against a
#      remote server where impairing all traffic on it is acceptable).
#
#   2. Network-namespace mode (isolates the impairment to just this test):
#        sudo ./netem_profiles.sh setup-netns
#        sudo ./netem_profiles.sh apply-netns <profile>
#        sudo ./netem_profiles.sh teardown-netns
#      Creates ns_server/ns_client + a veth pair, so only traffic between
#      the ingest engine (run inside ns_server) and your test client (run
#      inside ns_client) is affected -- nothing else on the host is touched.
#
# Profiles (delay/jitter/loss/reorder chosen to span "fine" to "clearly
# broken without a jitter buffer"):
#
#   clean                baseline, no impairment (for A/B comparison)
#   good-wifi             20ms +/-5ms jitter, 0.1% loss
#   bad-wifi               80ms +/-30ms jitter, 1% loss, 0.5% reorder
#   congested-cellular    150ms +/-60ms jitter, 3% loss, 1% reorder
#   satellite             550ms +/-20ms jitter, 0.5% loss  (high delay, low jitter)

set -euo pipefail

profile_args() {
    case "$1" in
        clean)               echo "" ;;
        good-wifi)            echo "delay 20ms 5ms distribution normal loss 0.1%" ;;
        bad-wifi)              echo "delay 80ms 30ms distribution normal loss 1% 25% reorder 0.5% 50%" ;;
        congested-cellular)   echo "delay 150ms 60ms distribution normal loss 3% 25% reorder 1% 50%" ;;
        satellite)            echo "delay 550ms 20ms distribution normal loss 0.5%" ;;
        *) echo "unknown profile: $1" >&2; exit 1 ;;
    esac
}

usage() {
    cat <<EOF
Usage:
  $0 apply <iface> <profile>      apply a profile directly to an interface
  $0 clear <iface>                remove netem qdisc from an interface
  $0 list                         list available profiles
  $0 setup-netns                  create ns_server/ns_client + veth pair
  $0 apply-netns <profile>        apply a profile to the veth inside ns_server
  $0 teardown-netns                remove the namespaces and veth pair

Profiles: clean good-wifi bad-wifi congested-cellular satellite
EOF
}

cmd="${1:-}"
case "$cmd" in
    list)
        echo "clean good-wifi bad-wifi congested-cellular satellite"
        ;;
    apply)
        iface="${2:?iface required}"; profile="${3:?profile required}"
        args="$(profile_args "$profile")"
        tc qdisc del dev "$iface" root 2>/dev/null || true
        if [ -n "$args" ]; then
            tc qdisc add dev "$iface" root netem $args
            echo "applied '$profile' to $iface: netem $args"
        else
            echo "profile 'clean': no impairment applied (qdisc cleared)"
        fi
        ;;
    clear)
        iface="${2:?iface required}"
        tc qdisc del dev "$iface" root 2>/dev/null || true
        echo "cleared netem from $iface"
        ;;
    setup-netns)
        ip netns add ns_server 2>/dev/null || true
        ip netns add ns_client 2>/dev/null || true
        ip link add veth0 type veth peer name veth1 2>/dev/null || true
        ip link set veth0 netns ns_server 2>/dev/null || true
        ip link set veth1 netns ns_client 2>/dev/null || true
        ip netns exec ns_server ip addr add 10.0.0.1/24 dev veth0 2>/dev/null || true
        ip netns exec ns_client ip addr add 10.0.0.2/24 dev veth1 2>/dev/null || true
        ip netns exec ns_server ip link set veth0 up
        ip netns exec ns_client ip link set veth1 up
        ip netns exec ns_server ip link set lo up
        ip netns exec ns_client ip link set lo up
        echo "namespaces ready. Run the ingest engine with:"
        echo "  ip netns exec ns_server ./build/ll_cmaf_ingest ..."
        echo "and your ffmpeg source / player with:"
        echo "  ip netns exec ns_client ffmpeg ... rtp://10.0.0.1:5004"
        echo "  ip netns exec ns_client curl http://10.0.0.1:8080/live.cmfv"
        ;;
    apply-netns)
        profile="${2:?profile required}"
        args="$(profile_args "$profile")"
        ip netns exec ns_server tc qdisc del dev veth0 root 2>/dev/null || true
        if [ -n "$args" ]; then
            ip netns exec ns_server tc qdisc add dev veth0 root netem $args
            echo "applied '$profile' inside ns_server: netem $args"
        else
            echo "profile 'clean': no impairment applied"
        fi
        ;;
    teardown-netns)
        ip link del veth0 2>/dev/null || true
        ip netns del ns_server 2>/dev/null || true
        ip netns del ns_client 2>/dev/null || true
        echo "namespaces and veth pair removed"
        ;;
    *)
        usage
        exit 1
        ;;
esac
