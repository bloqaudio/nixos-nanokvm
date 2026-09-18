"""Check CPUFreq and thermal integration in each default carrier DTB."""
import subprocess
import sys


def get(blob, node, prop, kind="u"):
    return subprocess.check_output(
        ["fdtget", "-t", kind, blob, node, prop], text=True
    ).strip()


def tree(blob, node="/"):
    result = {}
    props = subprocess.check_output(["fdtget", "-p", blob, node], text=True).split()
    result[node] = {p: get(blob, node, p, "bx") for p in props}
    for child in subprocess.check_output(["fdtget", "-l", blob, node], text=True).split():
        result.update(tree(blob, node.rstrip("/") + "/" + child))
    return result


scaling, = sys.argv[1:]
after = tree(scaling)

cpu = "/cpus/cpu@0"
assert "cpu-supply" not in after[cpu], "must not invent voltage control"
clock_provider, clock_id = get(scaling, cpu, "clocks").split()
assert clock_provider == get(scaling, "/soc/clock-controller@3002000", "phandle")
assert int(clock_id) == 155  # CLK_C906_0 in the pinned clock binding
assert get(scaling, cpu, "#cooling-cells") == "2"
assert get(scaling, cpu, "operating-points-v2") == get(scaling, "/opp-table-cpu", "phandle")
assert get(scaling, "/opp-table-cpu", "compatible", "s") == "operating-points-v2"
opp_nodes = [node for node in after if node.startswith("/opp-table-cpu/opp-")]
rates = []
for node in opp_nodes:
    hi, lo = map(int, get(scaling, node, "opp-hz").split())
    rates.append((hi << 32) | lo)
    assert "opp-microvolt" not in after[node]
    assert get(scaling, node, "clock-latency-ns") == "100000"
assert sorted(rates) == [212500000, 425000000, 850000000]
zone = "/thermal-zones/soc-thermal"
trip = zone + "/trips/cpu-passive"
assert get(scaling, trip, "temperature") == "85000"
assert get(scaling, trip, "hysteresis") == "5000"
assert get(scaling, trip, "type", "s") == "passive"
cooling = zone + "/cooling-maps/cpu-map"
assert get(scaling, cooling, "trip") == get(scaling, trip, "phandle")
assert get(scaling, cooling, "cooling-device").split() == [
    get(scaling, cpu, "phandle"), "4294967295", "4294967295"
]
print("Default CPUFreq DT: exact OPPs and CPU thermal link")
