#!/usr/bin/env python3
"""
xe_fan_watch - Intel Arc B580 Fan Control Update Monitor

Monitors multiple sources for fan control updates:
1. Local sysfs: checks if late binding firmware gets provisioned
2. CachyOS packages: checks for kernel updates
3. drm-xe GitLab: checks for fan/pwm/hwmon/late_bind merge requests
4. intel-xe mailing list: checks for fan control patches
5. Phoronix: checks for Intel xe fan control articles
6. GitHub compute-runtime: checks issue #885 for fan control updates

Sends desktop notifications when updates are found.
Stores results in JSON for the web dashboard.

Usage:
  python3 xe_fan_watch.py                # Run once, check all sources
  python3 xe_fan_watch.py --daemon        # Run continuously (checks every hour)
  python3 xe_fan_watch.py --notify        # Send desktop notification on new findings
  python3 xe_fan_watch.py --source local  # Check only local sysfs
  python3 xe_fan_watch.py --source all    # Check all sources (default)
  python3 xe_fan_watch.py --interval 3600 # Custom interval for daemon mode (seconds)
"""

import json
import os
import sys
import time
import subprocess
import argparse
import hashlib
import re
from datetime import datetime, timezone
from pathlib import Path
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError

from pci_utils import find_b580_pci_bdf, lb_fan_version_glob, xe_hwmon_path

# --- Configuration ---

DATA_FILE = Path(__file__).parent / "watch_data.json"
CONFIG = {
    # CachyOS / Arch kernel package (optional; skipped if not installed)
    "cachyos_package": "linux-cachyos",
    "cachyos_repo": "https://archlinux.cachyos.org",

    # drm-xe GitLab
    "gitlab_api": "https://gitlab.freedesktop.org/api/v4",
    "gitlab_project": "drm/xe/kernel",
    "gitlab_keywords": ["fan", "pwm", "hwmon", "late_bind", "late binding", "fan control"],

    # intel-xe mailing list
    "mailing_list_url": "https://lists.freedesktop.org/archives/intel-xe/",

    # Phoronix
    "phoronix_rss": "https://www.phoronix.com/rss.php",

    # GitHub compute-runtime
    "github_api": "https://api.github.com/repos/intel/compute-runtime/issues/885",
    "github_search": "https://api.github.com/search/issues?q=repo:intel/compute-runtime+fan+control",

    # Request settings
    "timeout": 15,
    "user_agent": "xe_fan_watch/1.0 (Linux fan control monitor)",
}

# --- Utilities ---

def now_iso():
    return datetime.now(timezone.utc).isoformat()

def load_data():
    if DATA_FILE.exists():
        try:
            return json.loads(DATA_FILE.read_text())
        except:
            pass
    return {"checks": [], "first_run": now_iso()}

def save_data(data):
    data["last_updated"] = now_iso()
    DATA_FILE.write_text(json.dumps(data, indent=2, default=str))

def content_hash(text):
    return hashlib.md5(text.encode()).hexdigest()[:12]

def fetch_url(url, timeout=None):
    timeout = timeout or CONFIG["timeout"]
    req = Request(url, headers={"User-Agent": CONFIG["user_agent"]})
    try:
        with urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8", errors="replace")
    except (URLError, HTTPError, Exception) as e:
        return None

def fetch_json(url, timeout=None):
    text = fetch_url(url, timeout)
    if text:
        try:
            return json.loads(text)
        except:
            pass
    return None

def send_notification(title, body, urgency="normal"):
    try:
        subprocess.run([
            "notify-send",
            "-u", urgency,
            "-t", "10000",
            title, body
        ], check=False, timeout=5)
    except:
        pass

# --- Source Checkers ---

def check_local_sysfs():
    """Check if late binding firmware is provisioned on this system."""
    result = {
        "source": "local_sysfs",
        "status": "unknown",
        "details": {},
        "timestamp": now_iso(),
        "found": False,
    }

    import glob

    pci_bdf = find_b580_pci_bdf()
    if pci_bdf:
        result["details"]["pci_bdf"] = pci_bdf

    # Check lb_fan_control_version
    paths = glob.glob(lb_fan_version_glob(pci_bdf))
    if paths:
        try:
            version = Path(paths[0]).read_text().strip()
            result["details"]["lb_fan_control_version"] = version
            result["details"]["path"] = paths[0]

            if version and version != "0.0.0.0":
                result["status"] = "provisioned"
                result["found"] = True
                result["message"] = f"FAN CONTROL FIRMWARE LOADED! Version: {version}"
            else:
                result["status"] = "not_provisioned"
                result["message"] = "Firmware not provisioned (version 0.0.0.0)"
        except:
            result["status"] = "error"
            result["message"] = "Cannot read lb_fan_control_version"
    else:
        result["status"] = "not_found"
        result["message"] = "lb_fan_control_version sysfs path not found"

    # Check current fan RPM and temp
    fan_path = xe_hwmon_path("fan1_input")
    temp_path = xe_hwmon_path("temp2_input")

    try:
        rpm = Path(fan_path).read_text().strip() if fan_path else ""
        result["details"]["fan_rpm"] = rpm or "unknown"
    except:
        result["details"]["fan_rpm"] = "unknown"

    try:
        if temp_path:
            temp_raw = int(Path(temp_path).read_text().strip())
            result["details"]["gpu_temp_c"] = temp_raw / 1000
        else:
            result["details"]["gpu_temp_c"] = "unknown"
    except:
        result["details"]["gpu_temp_c"] = "unknown"

    # Check installed kernel version
    try:
        kernel_ver = subprocess.check_output(["uname", "-r"], text=True).strip()
        result["details"]["kernel_version"] = kernel_ver
    except:
        result["details"]["kernel_version"] = "unknown"

    # Check installed package version
    try:
        pkg_ver = subprocess.check_output(
            ["pacman", "-Q", CONFIG["cachyos_package"]],
            text=True, stderr=subprocess.DEVNULL
        ).strip()
        result["details"]["package_version"] = pkg_ver
    except:
        result["details"]["package_version"] = "unknown"

    return result


def check_cachyos_updates():
    """Check if a newer CachyOS kernel package is available."""
    result = {
        "source": "cachyos_packages",
        "status": "unknown",
        "details": {},
        "timestamp": now_iso(),
        "found": False,
    }

    try:
        # Get installed version
        installed = subprocess.check_output(
            ["pacman", "-Q", CONFIG["cachyos_package"]],
            text=True, stderr=subprocess.DEVNULL
        ).strip().split()[1]

        # Check for updates
        sync_out = subprocess.check_output(
            ["pacman", "-Qu", CONFIG["cachyos_package"]],
            text=True, stderr=subprocess.DEVNULL
        ).strip()

        result["details"]["installed"] = installed

        if sync_out:
            # Parse update info
            parts = sync_out.split("->")
            if len(parts) >= 2:
                available = parts[-1].strip()
                result["details"]["available"] = available
                result["status"] = "update_available"
                result["found"] = True
                result["message"] = f"Kernel update available: {installed} -> {available}"
        else:
            result["details"]["available"] = installed
            result["status"] = "up_to_date"
            result["message"] = f"Kernel is up to date ({installed})"

    except subprocess.CalledProcessError:
        result["status"] = "no_updates"
        result["message"] = "No updates available"
    except Exception as e:
        result["status"] = "error"
        result["message"] = f"Error checking packages: {e}"

    return result


def check_gitlab_mrs():
    """Check drm-xe GitLab for fan/pwm/hwmon related merge requests."""
    result = {
        "source": "gitlab_mrs",
        "status": "unknown",
        "details": {"merge_requests": []},
        "timestamp": now_iso(),
        "found": False,
    }

    project_encoded = CONFIG["gitlab_project"].replace("/", "%2F")

    # Search for recent merge requests with fan-related keywords
    for keyword in CONFIG["gitlab_keywords"]:
        url = (
            f"{CONFIG['gitlab_api']}/projects/{project_encoded}"
            f"/merge_requests?search={keyword}&state=opened&per_page=20"
        )
        data = fetch_json(url)
        if data and isinstance(data, list):
            for mr in data:
                title = mr.get("title", "")
                iid = mr.get("iid", "")
                web_url = mr.get("web_url", "")
                created = mr.get("created_at", "")
                state = mr.get("state", "")

                # Avoid duplicates
                existing_iids = [m["iid"] for m in result["details"]["merge_requests"]]
                if iid not in existing_iids:
                    mr_info = {
                        "iid": iid,
                        "title": title,
                        "url": web_url,
                        "created_at": created,
                        "state": state,
                        "keyword": keyword,
                    }
                    result["details"]["merge_requests"].append(mr_info)

    mr_count = len(result["details"]["merge_requests"])
    if mr_count > 0:
        result["status"] = "found"
        result["found"] = True
        result["message"] = f"Found {mr_count} relevant merge request(s)"
    else:
        result["status"] = "none_found"
        result["message"] = "No fan-related merge requests found"

    return result


def check_mailing_list():
    """Check intel-xe mailing list archives for fan control patches."""
    result = {
        "source": "mailing_list",
        "status": "unknown",
        "details": {"posts": []},
        "timestamp": now_iso(),
        "found": False,
    }

    # Fetch the mailing list archive page
    html = fetch_url(CONFIG["mailing_list_url"])
    if not html:
        result["status"] = "error"
        result["message"] = "Cannot fetch mailing list page"
        return result

    # Look for links to recent monthly archives
    archive_links = re.findall(r'href="(\d{4}-\w+/)"', html)
    if archive_links:
        # Check the most recent few months
        archive_links = sorted(archive_links, reverse=True)[:3]

        for archive_link in archive_links:
            month_url = CONFIG["mailing_list_url"] + archive_link
            month_html = fetch_url(month_url)
            if not month_html:
                continue

            # Search for fan-related subjects
            for keyword in ["fan", "pwm", "hwmon", "late.bind"]:
                pattern = re.compile(
                    rf'<a[^>]+href="[^"]*"[^>]*>([^<]*{keyword}[^<]*)</a>',
                    re.IGNORECASE
                )
                matches = pattern.findall(month_html)
                for match in matches[:5]:
                    subject = match.strip()
                    if subject and len(subject) > 5:
                        result["details"]["posts"].append({
                            "subject": subject,
                            "month": archive_link.rstrip("/"),
                            "keyword": keyword,
                        })

    post_count = len(result["details"]["posts"])
    if post_count > 0:
        result["status"] = "found"
        result["found"] = True
        result["message"] = f"Found {post_count} relevant mailing list post(s)"
    else:
        result["status"] = "none_found"
        result["message"] = "No fan-related mailing list posts found"

    return result


def check_phoronix():
    """Check Phoronix RSS for Intel xe fan control articles."""
    result = {
        "source": "phoronix",
        "status": "unknown",
        "details": {"articles": []},
        "timestamp": now_iso(),
        "found": False,
    }

    rss = fetch_url(CONFIG["phoronix_rss"])
    if not rss:
        result["status"] = "error"
        result["message"] = "Cannot fetch Phoronix RSS"
        return result

    # Parse RSS items (simple regex, no feedparser dependency)
    items = re.findall(r'<item>(.*?)</item>', rss, re.DOTALL)
    for item in items:
        title_match = re.search(r'<title>(.*?)</title>', item, re.DOTALL)
        link_match = re.search(r'<link>(.*?)</link>', item, re.DOTALL)
        date_match = re.search(r'<pubDate>(.*?)</pubDate>', item, re.DOTALL)

        if title_match:
            title = title_match.group(1).strip()
            link = link_match.group(1).strip() if link_match else ""
            date = date_match.group(1).strip() if date_match else ""

            # Check for relevant keywords
            title_lower = title.lower()
            if any(kw in title_lower for kw in ["xe", "intel", "fan", "battlemage", "b580", "arc"]):
                if any(kw in title_lower for kw in ["fan", "pwm", "cool", "thermal", "hwmon"]):
                    result["details"]["articles"].append({
                        "title": title,
                        "url": link,
                        "date": date,
                    })

    article_count = len(result["details"]["articles"])
    if article_count > 0:
        result["status"] = "found"
        result["found"] = True
        result["message"] = f"Found {article_count} relevant Phoronix article(s)"
    else:
        result["status"] = "none_found"
        result["message"] = "No relevant Phoronix articles found"

    return result


def check_github_issue():
    """Check GitHub compute-runtime issue #885 for fan control updates."""
    result = {
        "source": "github_compute_runtime",
        "status": "unknown",
        "details": {},
        "timestamp": now_iso(),
        "found": False,
    }

    # Check the specific issue #885
    issue = fetch_json(CONFIG["github_api"])
    if issue:
        result["details"]["issue_title"] = issue.get("title", "")
        result["details"]["issue_url"] = issue.get("html_url", "")
        result["details"]["issue_state"] = issue.get("state", "")
        result["details"]["comments"] = issue.get("comments", 0)
        result["details"]["updated_at"] = issue.get("updated_at", "")

        if issue.get("state") == "closed":
            result["status"] = "issue_closed"
            result["found"] = True
            result["message"] = f"Issue #885 CLOSED - fan control may be implemented!"
        else:
            result["status"] = "open"
            result["message"] = f"Issue #885 still open ({issue.get('comments', 0)} comments)"

    # Also search for newer issues
    search_data = fetch_json(CONFIG["github_search"] + "&sort=updated&order=desc&per_page=5")
    if search_data and "items" in search_data:
        recent_issues = []
        for item in search_data["items"][:5]:
            recent_issues.append({
                "number": item.get("number"),
                "title": item.get("title"),
                "url": item.get("html_url"),
                "state": item.get("state"),
                "updated_at": item.get("updated_at"),
            })
        result["details"]["recent_issues"] = recent_issues

    return result


# --- Main Logic ---

CHECKERS = {
    "local": check_local_sysfs,
    "cachyos": check_cachyos_updates,
    "gitlab": check_gitlab_mrs,
    "mailing_list": check_mailing_list,
    "phoronix": check_phoronix,
    "github": check_github_issue,
}

def run_checks(sources="all", notify=False):
    data = load_data()
    results = []
    new_findings = False

    if sources == "all":
        sources_to_check = list(CHECKERS.keys())
    else:
        sources_to_check = [s.strip() for s in sources.split(",") if s.strip() in CHECKERS]

    print(f"\n{'='*60}")
    print(f"  xe_fan_watch - Checking {len(sources_to_check)} source(s)")
    print(f"  Time: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"{'='*60}\n")

    for source_name in sources_to_check:
        checker = CHECKERS[source_name]
        print(f"  [{source_name:>12}] Checking...", end=" ", flush=True)

        try:
            result = checker()
        except Exception as e:
            result = {
                "source": source_name,
                "status": "error",
                "message": str(e),
                "found": False,
                "timestamp": now_iso(),
            }

        results.append(result)

        # Check if this is a new finding
        prev = None
        for old_check in reversed(data.get("checks", [])):
            if old_check.get("source") == source_name:
                prev = old_check
                break

        is_new = False
        if result.get("found"):
            if not prev or not prev.get("found"):
                is_new = True
                new_findings = True
            # Also check if content changed
            current_hash = content_hash(json.dumps(result.get("details", {}), sort_keys=True))
            if prev:
                prev_hash = content_hash(json.dumps(prev.get("details", {}), sort_keys=True))
                if current_hash != prev_hash and result.get("found"):
                    is_new = True
                    new_findings = True

        status_icon = {
            "found": "FOUND",
            "provisioned": "FIRMWARE LOADED!",
            "update_available": "UPDATE",
            "issue_closed": "CLOSED!",
            "up_to_date": "ok",
            "not_provisioned": "not yet",
            "none_found": "nothing",
            "open": "open",
            "no_updates": "no updates",
            "error": "ERROR",
            "not_found": "not found",
            "unknown": "?",
        }.get(result.get("status", "unknown"), "?")

        new_tag = " *** NEW ***" if is_new else ""
        print(f"{status_icon}{new_tag}")

        if result.get("message"):
            print(f"               {result['message']}")

        if is_new and result.get("found"):
            print(f"               DETAILS: {json.dumps(result.get('details', {}), indent=2)[:200]}")

    # Store results
    data.setdefault("checks", []).extend(results)
    # Keep only last 500 checks
    if len(data["checks"]) > 500:
        data["checks"] = data["checks"][-500:]

    save_data(data)

    # Summary
    found_count = sum(1 for r in results if r.get("found"))
    print(f"\n{'='*60}")
    print(f"  Summary: {found_count}/{len(results)} sources have updates")
    if new_findings:
        print(f"  *** NEW FINDINGS DETECTED ***")
    print(f"  Data saved to: {DATA_FILE}")
    print(f"{'='*60}\n")

    # Send notifications
    if notify and new_findings:
        for r in results:
            if r.get("found") and r.get("message"):
                urgency = "critical" if r.get("status") == "provisioned" else "normal"
                send_notification(
                    f"xe_fan_watch: {r['source']}",
                    r["message"],
                    urgency
                )

    return results


def daemon_mode(interval, notify):
    """Run continuously, checking at regular intervals."""
    print(f"Starting daemon mode (interval: {interval}s, notify: {notify})")
    print("Press Ctrl+C to stop\n")

    while True:
        try:
            run_checks(sources="all", notify=notify)
            print(f"Next check in {interval} seconds...\n")
            time.sleep(interval)
        except KeyboardInterrupt:
            print("\nDaemon stopped.")
            break
        except Exception as e:
            print(f"Error in daemon loop: {e}")
            time.sleep(60)  # Wait before retrying


def main():
    parser = argparse.ArgumentParser(
        description="xe_fan_watch - Monitor for Intel Arc B580 fan control updates"
    )
    parser.add_argument("--daemon", action="store_true",
                        help="Run continuously, checking every hour")
    parser.add_argument("--notify", action="store_true",
                        help="Send desktop notifications on new findings")
    parser.add_argument("--source", default="all",
                        choices=["all", "local", "cachyos", "gitlab", "mailing_list", "phoronix", "github"],
                        help="Which source to check (default: all)")
    parser.add_argument("--interval", type=int, default=3600,
                        help="Check interval in seconds for daemon mode (default: 3600)")
    parser.add_argument("--status", action="store_true",
                        help="Show last check results from saved data")
    parser.add_argument("--json", action="store_true",
                        help="Output results as JSON")

    args = parser.parse_args()

    if args.status:
        data = load_data()
        if args.json:
            print(json.dumps(data, indent=2, default=str))
        else:
            checks = data.get("checks", [])
            if not checks:
                print("No checks have been run yet.")
                return

            print(f"\nLast {min(20, len(checks))} checks:\n")
            for check in checks[-20:]:
                ts = check.get("timestamp", "?")[:19]
                source = check.get("source", "?")
                status = check.get("status", "?")
                msg = check.get("message", "")
                found = "FOUND" if check.get("found") else ""
                print(f"  {ts} [{source:>12}] {status:>15} {found:>6}  {msg}")
        return

    if args.daemon:
        daemon_mode(args.interval, args.notify)
    else:
        results = run_checks(sources=args.source, notify=args.notify)
        if args.json:
            print(json.dumps(results, indent=2, default=str))


if __name__ == "__main__":
    main()
