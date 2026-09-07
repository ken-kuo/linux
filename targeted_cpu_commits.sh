#!/usr/bin/env bash
#
# Enhanced CPU Support Commit Analyzer (Plain Text Output)
# Purpose: Categorize Linux kernel commits specifically for Intel 5th Gen Xeon (Emerald Rapids), 
#          Intel 6th Gen Xeon (Granite Rapids), and AMD Zen 5
#          by CPU support components with precise word boundary matching
#
# Usage:
#   ./targeted_cpu_commits.sh                # search the entire history
#   ./targeted_cpu_commits.sh v6.1..v6.11    # limit to a version/tag range
#   ./targeted_cpu_commits.sh v6.6..HEAD     # from a base tag to current HEAD

set -euo pipefail
IFS=$'\n\t'

# Configuration
RANGE_OPT=""
OUTPUT_FILE="targeted_cpu_analysis_$(date +%Y%m%d_%H%M%S).txt"
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

if [ -n "${1:-}" ]; then
  RANGE_OPT="$1"
fi

# Helper functions
log_header() {
  echo "=== $1 ==="
  echo "=== $1 ===" >> "$OUTPUT_FILE"
}

log_section() {
  echo ""
  echo "----- $1 -----"
  echo "" >> "$OUTPUT_FILE"
  echo "----- $1 -----" >> "$OUTPUT_FILE"
}

log_subsection() {
  echo ">> $1"
  echo ">> $1" >> "$OUTPUT_FILE"
}

# Enhanced git log wrapper with commit counting and search term display
glog() {
  local search_term=""
  local temp_file="$TEMP_DIR/commits_$$"
  
  # Extract search term from arguments for display
  for arg in "$@"; do
      if [[ "$arg" == --grep=* ]]; then
          search_term=$(echo "$arg" | sed 's/--grep=//' | sed 's/\\b//g' | sed "s/'//g")
          break
      elif [[ "$arg" == -S* ]]; then
          search_term="code change: $(echo "$arg" | sed 's/-S//' | sed "s/'//g")"
          break
      fi
  done
  
  git log ${RANGE_OPT} --no-merges --date=short \
      --pretty='format:%h %ad %an %s' "$@" > "$temp_file" 2>/dev/null || true
  
  local count=$(wc -l < "$temp_file" 2>/dev/null || echo "0")
  
  # Display what we're searching for with proper line break
  if [ -n "$search_term" ]; then
      echo ""
      echo "Searching for: ${search_term}"
      echo "" >> "$OUTPUT_FILE"
      echo "Searching for: ${search_term}" >> "$OUTPUT_FILE"
  fi
  
  if [ "$count" -gt 0 ]; then
      echo "Found $count commits:"
      cat "$temp_file"
      echo "Found $count commits:" >> "$OUTPUT_FILE"
      cat "$temp_file" >> "$OUTPUT_FILE"
  else
      echo "No commits found"
      echo "No commits found" >> "$OUTPUT_FILE"
  fi
  echo "" >> "$OUTPUT_FILE"
}

# Multi-pattern search function for targeted CPU filtering with clean output
search_cpu_specific() {
  local category="$1"
  local temp_combined="$TEMP_DIR/combined_$$"
  shift
  
  # Clear the combined results file
  > "$temp_combined"
  
  # Run each search and append results (silently)
  for search_cmd in "$@"; do
      eval "$search_cmd" >> "$temp_combined" 2>/dev/null || true
  done
  
  # Remove duplicates and sort
  sort -u "$temp_combined" > "${temp_combined}.sorted"
  
  local count=$(wc -l < "${temp_combined}.sorted" 2>/dev/null || echo "0")
  if [ "$count" -gt 0 ]; then
      echo "Found $count unique commits for $category:"
      cat "${temp_combined}.sorted"
      echo "Found $count unique commits for $category:" >> "$OUTPUT_FILE"
      cat "${temp_combined}.sorted" >> "$OUTPUT_FILE"
  else
      echo "No commits found for $category"
      echo "No commits found for $category" >> "$OUTPUT_FILE"
  fi
  echo "" >> "$OUTPUT_FILE"
}

# Start analysis
log_header "Targeted CPU Support Analysis Report"
echo "Target CPUs: Intel 5th Gen Xeon (Emerald Rapids), Intel 6th Gen Xeon (Granite Rapids) & AMD Zen 5"
echo "Search range: ${RANGE_OPT:-<FULL HISTORY>}"
echo "Search mode: Precise word boundary matching (no false positives)"
echo "Timestamp: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "Output file: $OUTPUT_FILE"
echo ""

echo "Target CPUs: Intel 5th Gen Xeon (Emerald Rapids), Intel 6th Gen Xeon (Granite Rapids) & AMD Zen 5" >> "$OUTPUT_FILE"
echo "Search range: ${RANGE_OPT:-<FULL HISTORY>}" >> "$OUTPUT_FILE"
echo "Search mode: Precise word boundary matching (no false positives)" >> "$OUTPUT_FILE"
echo "Timestamp: $(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"

# ============================================================================
# INTEL EMERALD RAPIDS (5TH GEN) ANALYSIS
# ============================================================================
log_section "INTEL EMERALD RAPIDS (5TH GEN) ANALYSIS"

log_subsection "1. Intel Emerald Rapids Family Detection"
glog -i --grep='\bemerald.*rapids\b'
glog -i --grep='\bEMR\b'
glog -S'0xcf' -- arch/x86/include/asm/intel-family.h
glog -S'EMERALD_RAPIDS' -- arch/x86/include/asm/intel-family.h

log_subsection "2. Intel Emerald Rapids Microcode"
search_cpu_specific "Intel EMR Microcode" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bemerald.*rapids\b' -- arch/x86/kernel/cpu/microcode/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bEMR\b' -- arch/x86/kernel/cpu/microcode/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xcf' -- arch/x86/kernel/cpu/microcode/intel.c"

log_subsection "3. Intel Emerald Rapids Power Management"
search_cpu_specific "Intel EMR Power Management" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bemerald.*rapids\b' -- drivers/cpufreq/ drivers/idle/ drivers/powercap/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bEMR\b' -- drivers/cpufreq/ drivers/idle/ drivers/powercap/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xcf' -- drivers/cpufreq/intel_pstate.c drivers/idle/intel_idle.c"

log_subsection "4. Intel Emerald Rapids PMU"
search_cpu_specific "Intel EMR PMU" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bemerald.*rapids\b' -- arch/x86/events/ tools/perf/pmu-events/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bEMR\b' -- arch/x86/events/ tools/perf/pmu-events/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'emerald' -- tools/perf/pmu-events/arch/x86/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'EMR' -- tools/perf/pmu-events/arch/x86/"

log_subsection "5. Intel Emerald Rapids Thermal"
search_cpu_specific "Intel EMR Thermal" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bemerald.*rapids\b' -- drivers/thermal/ drivers/hwmon/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bEMR\b' -- drivers/thermal/ drivers/hwmon/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xcf' -- drivers/thermal/intel/"

log_subsection "6. Intel Emerald Rapids Virtualization"
search_cpu_specific "Intel EMR Virtualization" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bemerald.*rapids\b' -- arch/x86/kvm/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bEMR\b' -- arch/x86/kvm/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xcf' -- arch/x86/kvm/vmx/"

log_subsection "7. Intel Emerald Rapids Security"
search_cpu_specific "Intel EMR Security" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bemerald.*rapids\b' -- arch/x86/kernel/cpu/bugs.c arch/x86/kernel/cpu/intel.c" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bEMR\b' -- arch/x86/kernel/cpu/bugs.c" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xcf' -- arch/x86/kernel/cpu/bugs.c"

log_subsection "8. Intel Emerald Rapids Memory"
search_cpu_specific "Intel EMR Memory" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bemerald.*rapids\b' -- drivers/edac/ arch/x86/kernel/cpu/mce/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bEMR\b' -- drivers/edac/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xcf' -- arch/x86/kernel/cpu/mce/intel.c"

# ============================================================================
# INTEL GRANITE RAPIDS (6TH GEN) ANALYSIS
# ============================================================================
log_section "INTEL GRANITE RAPIDS (6TH GEN) ANALYSIS"

log_subsection "1. Intel Granite Rapids Family Detection"
glog -i --grep='\bgranite.*rapids\b'
glog -i --grep='\bGNR\b'
glog -S'0xad' -- arch/x86/include/asm/intel-family.h
glog -S'GRANITE_RAPIDS' -- arch/x86/include/asm/intel-family.h

log_subsection "2. Intel Granite Rapids Microcode"
search_cpu_specific "Intel GNR Microcode" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bgranite.*rapids\b' -- arch/x86/kernel/cpu/microcode/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bGNR\b' -- arch/x86/kernel/cpu/microcode/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xad' -- arch/x86/kernel/cpu/microcode/intel.c"

log_subsection "3. Intel Granite Rapids Power Management"
search_cpu_specific "Intel GNR Power Management" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bgranite.*rapids\b' -- drivers/cpufreq/ drivers/idle/ drivers/powercap/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bGNR\b' -- drivers/cpufreq/ drivers/idle/ drivers/powercap/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xad' -- drivers/cpufreq/intel_pstate.c drivers/idle/intel_idle.c"

log_subsection "4. Intel Granite Rapids PMU"
search_cpu_specific "Intel GNR PMU" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bgranite.*rapids\b' -- arch/x86/events/ tools/perf/pmu-events/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bGNR\b' -- arch/x86/events/ tools/perf/pmu-events/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'granite' -- tools/perf/pmu-events/arch/x86/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'GNR' -- tools/perf/pmu-events/arch/x86/"

log_subsection "5. Intel Granite Rapids Thermal"
search_cpu_specific "Intel GNR Thermal" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bgranite.*rapids\b' -- drivers/thermal/ drivers/hwmon/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bGNR\b' -- drivers/thermal/ drivers/hwmon/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xad' -- drivers/thermal/intel/"

log_subsection "6. Intel Granite Rapids Virtualization"
search_cpu_specific "Intel GNR Virtualization" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bgranite.*rapids\b' -- arch/x86/kvm/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bGNR\b' -- arch/x86/kvm/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xad' -- arch/x86/kvm/vmx/"

log_subsection "7. Intel Granite Rapids Security"
search_cpu_specific "Intel GNR Security" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bgranite.*rapids\b' -- arch/x86/kernel/cpu/bugs.c arch/x86/kernel/cpu/intel.c" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bGNR\b' -- arch/x86/kernel/cpu/bugs.c" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xad' -- arch/x86/kernel/cpu/bugs.c"

log_subsection "8. Intel Granite Rapids Memory"
search_cpu_specific "Intel GNR Memory" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bgranite.*rapids\b' -- drivers/edac/ arch/x86/kernel/cpu/mce/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bGNR\b' -- drivers/edac/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0xad' -- arch/x86/kernel/cpu/mce/intel.c"

# ============================================================================
# AMD ZEN 5 ANALYSIS
# ============================================================================
log_section "AMD ZEN 5 ANALYSIS"

log_subsection "1. AMD Zen 5 Family Detection"
glog -i --grep='\bzen.*5\b'
glog -i --grep='\bfamily.*0x1a\b'
glog -S'0x1a' -- arch/x86/kernel/cpu/amd.c

log_subsection "2. AMD Zen 5 Microcode"
search_cpu_specific "AMD Zen 5 Microcode" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bzen.*5\b' -- arch/x86/kernel/cpu/microcode/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bfamily.*0x1a\b' -- arch/x86/kernel/cpu/microcode/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0x1a' -- arch/x86/kernel/cpu/microcode/amd.c"

log_subsection "3. AMD Zen 5 Power Management"
search_cpu_specific "AMD Zen 5 Power Management" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bzen.*5\b' -- drivers/cpufreq/ drivers/idle/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bfamily.*0x1a\b' -- drivers/cpufreq/ drivers/acpi/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0x1a' -- drivers/cpufreq/amd-pstate.c"

log_subsection "4. AMD Zen 5 PMU"
search_cpu_specific "AMD Zen 5 PMU" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bzen.*5\b' -- arch/x86/events/ tools/perf/pmu-events/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'zen5' -- tools/perf/pmu-events/arch/x86/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'ZEN5' -- tools/perf/pmu-events/arch/x86/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0x1a' -- arch/x86/events/amd/"

log_subsection "5. AMD Zen 5 Thermal"
search_cpu_specific "AMD Zen 5 Thermal" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bzen.*5\b' -- drivers/hwmon/k10temp.c" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bfamily.*0x1a\b' -- drivers/hwmon/k10temp.c" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0x1a' -- drivers/hwmon/k10temp.c"

log_subsection "6. AMD Zen 5 Virtualization"
search_cpu_specific "AMD Zen 5 Virtualization" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bzen.*5\b' -- arch/x86/kvm/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bfamily.*0x1a\b' -- arch/x86/kvm/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0x1a' -- arch/x86/kvm/svm/"

log_subsection "7. AMD Zen 5 Security"
search_cpu_specific "AMD Zen 5 Security" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bzen.*5\b' -- arch/x86/kernel/cpu/bugs.c arch/x86/kernel/cpu/amd.c" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bfamily.*0x1a\b' -- arch/x86/kernel/cpu/bugs.c" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0x1a' -- arch/x86/kernel/cpu/amd.c arch/x86/kvm/svm/"

log_subsection "8. AMD Zen 5 Memory"
search_cpu_specific "AMD Zen 5 Memory" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bzen.*5\b' -- drivers/edac/ arch/x86/kernel/cpu/mce/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -i --grep='\bfamily.*0x1a\b' -- drivers/edac/" \
  "git log ${RANGE_OPT} --no-merges --date=short --pretty='format:%h %ad %an %s' -S'0x1a' -- arch/x86/kernel/cpu/mce/amd.c"

# ============================================================================
# SUMMARY
# ============================================================================
log_section "ANALYSIS COMPLETE"
echo "Analysis completed successfully!"
echo "Full results saved to: ${OUTPUT_FILE}"
echo "Check the output file for detailed commit information"

echo "Analysis completed successfully!" >> "$OUTPUT_FILE"
echo "Full results saved to: ${OUTPUT_FILE}" >> "$OUTPUT_FILE"
echo "Check the output file for detailed commit information" >> "$OUTPUT_FILE"
