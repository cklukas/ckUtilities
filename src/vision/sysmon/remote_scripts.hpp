// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>
namespace ck::sysmon {
// Constant scripts only; host aliases are separate SSH argument data.
inline constexpr std::string_view linux_script = R"CKSYS(
export LC_ALL=C
printf 'CKSYSMON 2\nSYSTEM\tLinux / %s' "$(uname -m)"
awk '/^cpu[0-9]+ / {n++} END {if(n>0)printf " / %d threads",n; printf "\n"}' /proc/stat
awk '/^cpu / {t=0; for(i=2;i<=9;i++)t+=$i; printf "CPU\t%.0f\t%.0f\n",t,$5+$6; exit}' /proc/stat
awk '/^MemTotal:/ {t=$2*1024} /^MemAvailable:/ {a=$2*1024; ok=1} END {if(ok && t>=a)printf "RAM\t%.0f\t%.0f\n",t-a,t; else print "RAM\t-\t-"}' /proc/meminfo
if command -v nvidia-smi >/dev/null 2>&1; then
nvidia-smi --query-gpu=uuid,name,utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw --format=csv,noheader,nounits 2>/dev/null | awk -F',' '{for(i=1;i<=NF;i++)gsub(/^ +| +$/,"",$i); if(NF==7){u=$3 ~ /^[0-9.]+$/ ? $3 : "-"; m=$4 ~ /^[0-9.]+$/ ? sprintf("%.0f",$4*1048576) : "-"; t=$5 ~ /^[0-9.]+$/ ? sprintf("%.0f",$5*1048576) : "-"; c=$6 ~ /^[0-9.]+$/ ? $6 : "-"; w=$7 ~ /^[0-9.]+$/ ? $7 : "-"; printf "GPU\t%s\t%s\t%s\t%s\t%s\t-\t%s\t%s\tdedicated\n",$1,$2,u,m,t,c,w}}'
fi
for d in /sys/class/drm/card[0-9] /sys/class/drm/card[0-9][0-9]; do
driver=$(readlink "$d/device/driver")
if [ "${driver##*/}" = v3d ]; then
m=-; n=${d##*/card}
[ ! -r "/sys/kernel/debug/dri/$n/bo_stats" ] || m=$(awk '/^allocated bo size \(kb\):/ {if($5 ~ /^[0-9]+$/)printf "%.0f",$5*1024}' "/sys/kernel/debug/dri/$n/bo_stats")
[ -n "$m" ] || m=-
printf 'GPU\t%s\tV3D %s\t-\t%s\t-\t-\t-\t-\tv3d-buffers\n' "$d" "${d##*/}" "$m"
[ ! -r "$d/device/gpu_stats" ] || awk -v id="$d" 'NR==1 {ok=($1=="queue" && $2=="timestamp" && $3=="jobs" && $4=="runtime"); next} ok && NF==4 && $1 ~ /^(bin|render|tfu|csd)$/ && $2 ~ /^[0-9]+$/ && $4 ~ /^[0-9]+$/ {printf "GPU_ENGINE\t%s\t%s\t%s\t%s\n",id,$1,$2,$4}' "$d/device/gpu_stats"
continue
fi
[ -r "$d/device/vendor" ] || continue
v=$(cat "$d/device/vendor"); [ "$v" = 0x10de ] && continue
u=-; m=-; t=-; s=-
[ -r "$d/device/gpu_busy_percent" ] && u=$(cat "$d/device/gpu_busy_percent")
[ -r "$d/device/mem_info_vram_used" ] && m=$(cat "$d/device/mem_info_vram_used")
[ -r "$d/device/mem_info_vram_total" ] && t=$(cat "$d/device/mem_info_vram_total")
[ -r "$d/device/mem_info_gtt_used" ] && s=$(cat "$d/device/mem_info_gtt_used")
printf 'GPU\t%s\t%s\t%s\t%s\t%s\t%s\t-\t-\tdedicated\n' "$d" "GPU ${d##*/} ($v)" "$u" "$m" "$t" "$s"
done
printf 'OLLAMA\tnot running\n'
if command -v curl >/dev/null 2>&1; then
j=$(curl --noproxy '*' --max-time 1 --max-filesize 32768 -fsS http://127.0.0.1:11434/api/ps 2>/dev/null) && printf 'OLLAMA_JSON\t%s\n' "$j"
fi
printf 'END\n'
)CKSYS";
inline constexpr std::string_view darwin_script = R"CKSYS(
export LC_ALL=C
printf 'CKSYSMON 2\nSYSTEM\tmacOS / %s / %s threads\n' "$(uname -m)" "$(/usr/sbin/sysctl -n hw.logicalcpu)"
# Aggregate interval CPU load through a fresh top interval; never report loadavg as percent.
top -l 2 -s 1 -n 0 2>/dev/null | awk '/CPU usage:/ {idle=$7; gsub(/%/,"",idle)} END {if(idle!="")printf "LOAD\t%.2f\n",100-idle}'
t=$(/usr/sbin/sysctl -n hw.memsize)
vm_stat | awk -v total="$t" '/page size of/ {p=$8} /^Pages free:/ {f=$3} /^Pages inactive:/ {i=$3} END {gsub(/\./,"",f); gsub(/\./,"",i); if(p>0 && total>0)printf "RAM\t%.0f\t%.0f\n",total-(f+i)*p,total; else print "RAM\t-\t-"}'
printf 'OLLAMA\tnot running\n'
if command -v curl >/dev/null 2>&1; then
j=$(curl --noproxy '*' --max-time 1 --max-filesize 32768 -fsS http://127.0.0.1:11434/api/ps 2>/dev/null) && printf 'OLLAMA_JSON\t%s\n' "$j"
fi
printf 'END\n'
)CKSYS";
// PowerShell's invariant formatting avoids localized number/counter names.
inline constexpr std::string_view windows_script = R"CKSYS(
$ErrorActionPreference='Stop'; [Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false)
[Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::InvariantCulture
Write-Output "CKSYSMON 2"
Write-Output "SYSTEM`tWindows / $env:PROCESSOR_ARCHITECTURE / $env:NUMBER_OF_PROCESSORS threads"
try { $c=Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'"; Write-Output "LOAD`t$($c.PercentProcessorTime)" } catch {}
try { $m=Get-CimInstance Win32_OperatingSystem; $t=[uint64]$m.TotalVisibleMemorySize*1024; $u=$t-[uint64]$m.FreePhysicalMemory*1024; Write-Output "RAM`t$u`t$t" } catch { Write-Output "RAM`t-`t-" }
# NVML's fallback memory view lacks WDDM shared-memory accounting. The
# native C++ helper resolves physical device identities and reads PDH instead.
try {
foreach($line in (& nvidia-smi --query-gpu=uuid,name,utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw --format=csv,noheader,nounits 2>$null)) {
$f=$line.Split(',') | ForEach-Object {$_.Trim()}; if($f.Count -ne 7){continue}
$u='-'; $m='-'; $t='-'; $c='-'; $w='-'
if($f[2] -match '^\d+(\.\d+)?$'){$u=$f[2]}; if($f[3] -match '^\d+(\.\d+)?$'){$m=[uint64]([double]$f[3]*1048576)}
if($f[4] -match '^\d+(\.\d+)?$'){$t=[uint64]([double]$f[4]*1048576)}; if($f[5] -match '^\d+(\.\d+)?$'){$c=$f[5]}; if($f[6] -match '^\d+(\.\d+)?$'){$w=$f[6]}
Write-Output "GPU`t$($f[0])`t$($f[1])`t$u`t$m`t$t`t-`t$c`t$w`tdedicated"
}
} catch {}
Write-Output "OLLAMA`tnot running"
try { $j=Invoke-RestMethod -Uri http://127.0.0.1:11434/api/ps -TimeoutSec 1; Write-Output ("OLLAMA_JSON`t"+($j | ConvertTo-Json -Depth 10 -Compress)) } catch {}
Write-Output 'END'
)CKSYS";
} // namespace ck::sysmon
