#!/usr/bin/env python3
"""
xe_fan_dashboard - Web Dashboard for Intel Arc B580 Fan Control Monitor

A lightweight web dashboard (no external dependencies, uses Python stdlib)
that displays the monitoring results from xe_fan_watch.py.

Features:
- Real-time status of all monitored sources
- Local GPU status (firmware version, fan RPM, temperature)
- Recent findings from GitLab, mailing list, Phoronix, GitHub
- Auto-refresh every 60 seconds
- Clean, dark-themed UI

Usage:
  python3 xe_fan_dashboard.py                # Start on port 8585
  python3 xe_fan_dashboard.py --port 8080    # Custom port
  python3 xe_fan_dashboard.py --host 0.0.0.0 # Listen on all interfaces
  python3 xe_fan_dashboard.py --check        # Run a check before starting

Then open http://localhost:8585 in your browser.
"""

import json
import os
import sys
import subprocess
import argparse
import threading
import time
import glob
from datetime import datetime, timezone
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

from pci_utils import find_b580_pci_bdf, lb_fan_version_glob, xe_hwmon_path

DATA_FILE = Path(__file__).parent / "watch_data.json"
WATCH_SCRIPT = Path(__file__).parent / "xe_fan_watch.py"

# --- Data Loading ---

def load_data():
    if DATA_FILE.exists():
        try:
            return json.loads(DATA_FILE.read_text())
        except:
            pass
    return {"checks": [], "first_run": datetime.now(timezone.utc).isoformat()}

def get_local_status():
    """Get current local GPU status."""
    status = {
        "firmware_version": "unknown",
        "firmware_provisioned": False,
        "fan_rpm": "unknown",
        "gpu_temp": "unknown",
        "kernel_version": "unknown",
    }

    pci_bdf = find_b580_pci_bdf()
    if pci_bdf:
        status["pci_bdf"] = pci_bdf

    # Firmware version
    paths = glob.glob(lb_fan_version_glob(pci_bdf))
    if paths:
        try:
            ver = Path(paths[0]).read_text().strip()
            status["firmware_version"] = ver
            status["firmware_provisioned"] = ver and ver != "0.0.0.0"
        except:
            pass

    # Fan RPM
    fan_path = xe_hwmon_path("fan1_input")
    try:
        if fan_path:
            status["fan_rpm"] = int(Path(fan_path).read_text().strip())
    except:
        pass

    # GPU temp
    temp_path = xe_hwmon_path("temp2_input")
    try:
        if temp_path:
            temp = int(Path(temp_path).read_text().strip())
            status["gpu_temp"] = temp / 1000
    except:
        pass

    # Kernel version
    try:
        status["kernel_version"] = subprocess.check_output(
            ["uname", "-r"], text=True
        ).strip()
    except:
        pass

    return status

def get_latest_results(data):
    """Get the most recent result for each source."""
    latest = {}
    for check in reversed(data.get("checks", [])):
        source = check.get("source")
        if source and source not in latest:
            latest[source] = check
    return latest

# --- Background Checker ---

def background_checker(interval=3600):
    """Run xe_fan_watch.py periodically in the background."""
    while True:
        try:
            subprocess.run(
                [sys.executable, str(WATCH_SCRIPT), "--source", "all"],
                capture_output=True, text=True, timeout=120
            )
        except:
            pass
        time.sleep(interval)

# --- HTML Template ---

def generate_html(data, local_status):
    latest = get_latest_results(data)
    now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    # Local status card
    fw_provisioned = local_status.get("firmware_provisioned", False)
    fw_class = "status-provisioned" if fw_provisioned else "status-not-provisioned"
    fw_text = "PROVISIONED" if fw_provisioned else "NOT PROVISIONED"
    fw_icon = "&#10004;" if fw_provisioned else "&#10006;"

    fan_rpm = local_status.get("fan_rpm", "?")
    gpu_temp = local_status.get("gpu_temp", "?")
    kernel_ver = local_status.get("kernel_version", "?")
    fw_ver = local_status.get("firmware_version", "0.0.0.0")

    # Source cards
    source_labels = {
        "local_sysfs": "Local Sysfs",
        "cachyos_packages": "CachyOS Packages",
        "gitlab_mrs": "drm-xe GitLab MRs",
        "mailing_list": "intel-xe Mailing List",
        "phoronix": "Phoronix",
        "github_compute_runtime": "GitHub compute-runtime",
    }

    source_icons = {
        "local_sysfs": "&#128204;",
        "cachyos_packages": "&#128230;",
        "gitlab_mrs": "&#128279;",
        "mailing_list": "&#9993;",
        "phoronix": "&#128240;",
        "github_compute_runtime": "&#128187;",
    }

    cards_html = ""
    for source_key, source_label in source_labels.items():
        result = latest.get(source_key, {})
        status = result.get("status", "unknown")
        found = result.get("found", False)
        message = result.get("message", "Not checked yet")
        timestamp = result.get("timestamp", "")[:19]
        details = result.get("details", {})

        status_class = "card-normal"
        if found:
            status_class = "card-found"
        elif status == "error":
            status_class = "card-error"
        elif status in ("up_to_date", "none_found", "not_provisioned", "open", "no_updates"):
            status_class = "card-ok"

        icon = source_icons.get(source_key, "&#128269;")

        # Build details section
        details_html = ""
        if details:
            if source_key == "gitlab_mrs" and details.get("merge_requests"):
                details_html = "<div class='details-list'>"
                for mr in details["merge_requests"][:5]:
                    details_html += f"<div class='detail-item'><a href='{mr.get('url','')}' target='_blank'>MR !{mr.get('iid','')}: {mr.get('title','')}</a></div>"
                details_html += "</div>"
            elif source_key == "mailing_list" and details.get("posts"):
                details_html = "<div class='details-list'>"
                for post in details["posts"][:5]:
                    details_html += f"<div class='detail-item'>{post.get('subject','')} ({post.get('month','')})</div>"
                details_html += "</div>"
            elif source_key == "phoronix" and details.get("articles"):
                details_html = "<div class='details-list'>"
                for art in details["articles"][:5]:
                    details_html += f"<div class='detail-item'><a href='{art.get('url','')}' target='_blank'>{art.get('title','')}</a></div>"
                details_html += "</div>"
            elif source_key == "github_compute_runtime":
                if details.get("issue_state"):
                    details_html = f"<div class='details-list'>"
                    details_html += f"<div class='detail-item'>Issue #885: {details.get('issue_state','')} ({details.get('comments',0)} comments)</div>"
                    if details.get("issue_url"):
                        details_html += f"<div class='detail-item'><a href='{details['issue_url']}' target='_blank'>View on GitHub</a></div>"
                    if details.get("recent_issues"):
                        for issue in details["recent_issues"][:3]:
                            details_html += f"<div class='detail-item'><a href='{issue.get('url','')}' target='_blank'>#{issue.get('number','')}: {issue.get('title','')}</a></div>"
                    details_html += "</div>"
            elif source_key == "cachyos_packages":
                details_html = f"<div class='details-list'>"
                details_html += f"<div class='detail-item'>Installed: {details.get('installed','?')}</div>"
                details_html += f"<div class='detail-item'>Available: {details.get('available','?')}</div>"
                details_html += "</div>"
            elif source_key == "local_sysfs":
                details_html = f"<div class='details-list'>"
                details_html += f"<div class='detail-item'>Kernel: {details.get('kernel_version','?')}</div>"
                details_html += f"<div class='detail-item'>Package: {details.get('package_version','?')}</div>"
                details_html += f"<div class='detail-item'>Fan RPM: {details.get('fan_rpm','?')}</div>"
                details_html += f"<div class='detail-item'>GPU Temp: {details.get('gpu_temp_c','?')}C</div>"
                details_html += "</div>"

        cards_html += f"""
        <div class="card {status_class}">
            <div class="card-header">
                <span class="card-icon">{icon}</span>
                <span class="card-title">{source_label}</span>
                <span class="card-status">{status}</span>
            </div>
            <div class="card-message">{message}</div>
            {details_html}
            <div class="card-timestamp">{timestamp}</div>
        </div>
        """

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>xe_fan_watch - B580 Fan Control Monitor</title>
    <meta http-equiv="refresh" content="60">
    <style>
        :root {{
            --bg: #0d1117;
            --card-bg: #161b22;
            --border: #30363d;
            --text: #c9d1d9;
            --text-dim: #8b949e;
            --accent: #58a6ff;
            --green: #3fb950;
            --red: #f85149;
            --yellow: #d29922;
            --orange: #db6d28;
        }}
        * {{ margin: 0; padding: 0; box-sizing: border-box; }}
        body {{
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif;
            background: var(--bg);
            color: var(--text);
            padding: 20px;
            max-width: 1200px;
            margin: 0 auto;
        }}
        h1 {{
            font-size: 1.8rem;
            margin-bottom: 5px;
            display: flex;
            align-items: center;
            gap: 10px;
        }}
        h1 .gpu-icon {{ font-size: 1.5rem; }}
        .subtitle {{
            color: var(--text-dim);
            font-size: 0.9rem;
            margin-bottom: 20px;
        }}
        .header-bar {{
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 20px;
        }}
        .refresh-info {{
            font-size: 0.8rem;
            color: var(--text-dim);
        }}
        .local-status {{
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 20px;
            display: grid;
            grid-template-columns: 1fr 1fr 1fr 1fr;
            gap: 15px;
        }}
        .local-status .label {{
            font-size: 0.75rem;
            color: var(--text-dim);
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }}
        .local-status .value {{
            font-size: 1.4rem;
            font-weight: 600;
            margin-top: 4px;
        }}
        .fw-status {{
            grid-column: span 4;
            text-align: center;
            padding: 12px;
            border-radius: 8px;
            font-weight: 700;
            font-size: 1.1rem;
            margin-bottom: 10px;
        }}
        .status-provisioned {{
            background: rgba(63, 185, 80, 0.15);
            color: var(--green);
            border: 1px solid var(--green);
        }}
        .status-not-provisioned {{
            background: rgba(248, 81, 73, 0.1);
            color: var(--red);
            border: 1px solid rgba(248, 81, 73, 0.3);
        }}
        .cards {{
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(340px, 1fr));
            gap: 15px;
        }}
        .card {{
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 10px;
            padding: 16px;
            transition: border-color 0.3s;
        }}
        .card-found {{ border-color: var(--yellow); }}
        .card-error {{ border-color: var(--red); }}
        .card-ok {{ border-color: var(--border); }}
        .card-header {{
            display: flex;
            align-items: center;
            gap: 8px;
            margin-bottom: 10px;
        }}
        .card-icon {{ font-size: 1.2rem; }}
        .card-title {{
            font-weight: 600;
            flex-grow: 1;
        }}
        .card-status {{
            font-size: 0.75rem;
            padding: 2px 8px;
            border-radius: 4px;
            background: var(--border);
            color: var(--text-dim);
        }}
        .card-found .card-status {{ background: rgba(210, 153, 34, 0.2); color: var(--yellow); }}
        .card-error .card-status {{ background: rgba(248, 81, 73, 0.2); color: var(--red); }}
        .card-message {{
            font-size: 0.9rem;
            margin-bottom: 10px;
            color: var(--text);
        }}
        .details-list {{
            margin-top: 8px;
            padding-top: 8px;
            border-top: 1px solid var(--border);
        }}
        .detail-item {{
            font-size: 0.85rem;
            padding: 3px 0;
            color: var(--text-dim);
        }}
        .detail-item a {{
            color: var(--accent);
            text-decoration: none;
        }}
        .detail-item a:hover {{ text-decoration: underline; }}
        .card-timestamp {{
            font-size: 0.7rem;
            color: var(--text-dim);
            margin-top: 8px;
            text-align: right;
        }}
        .footer {{
            margin-top: 30px;
            text-align: center;
            color: var(--text-dim);
            font-size: 0.8rem;
        }}
        .big-alert {{
            background: rgba(63, 185, 80, 0.1);
            border: 2px solid var(--green);
            border-radius: 12px;
            padding: 20px;
            text-align: center;
            margin-bottom: 20px;
            display: none;
        }}
        .big-alert.show {{ display: block; }}
        .big-alert h2 {{
            color: var(--green);
            font-size: 1.3rem;
            margin-bottom: 8px;
        }}
        @media (max-width: 768px) {{
            .local-status {{
                grid-template-columns: 1fr 1fr;
            }}
            .fw-status {{ grid-column: span 2; }}
            .cards {{
                grid-template-columns: 1fr;
            }}
        }}
    </style>
</head>
<body>
    <div class="header-bar">
        <h1><span class="gpu-icon">&#127918;</span> xe_fan_watch <span style="color:var(--text-dim);font-size:0.9rem;">Intel Arc B580 Fan Control Monitor</span></h1>
        <div class="refresh-info">Last updated: {now_str}<br>Auto-refresh: 60s</div>
    </div>

    <div class="subtitle">
        Monitoring 6 sources for fan control updates. When the firmware gets provisioned,
        you'll be able to use the fan control tools in this project.
    </div>

    {"<div class='big-alert show'><h2>&#127881; FAN CONTROL FIRMWARE IS PROVISIONED! &#127881;</h2><p>Run the probe tools to test fan control: <code>sudo ./src/userspace/xe_pcode_probe --all</code></p></div>" if fw_provisioned else ""}

    <div class="local-status">
        <div class="fw-status {fw_class}">
            {fw_icon} Fan Control Firmware: {fw_text} (v{fw_ver})
        </div>
        <div>
            <div class="label">Fan RPM</div>
            <div class="value">{fan_rpm}</div>
        </div>
        <div>
            <div class="label">GPU Temp</div>
            <div class="value">{gpu_temp}&deg;C</div>
        </div>
        <div>
            <div class="label">Kernel</div>
            <div class="value" style="font-size:1rem;">{kernel_ver}</div>
        </div>
        <div>
            <div class="label">Status</div>
            <div class="value" style="font-size:1rem;color:{'var(--green)' if fw_provisioned else 'var(--text-dim)'};">{'Ready!' if fw_provisioned else 'Waiting...'}</div>
        </div>
    </div>

    <div class="cards">
        {cards_html}
    </div>

    <div class="footer">
        xe_fan_watch v1.0 | <a href="/api/json" style="color:var(--accent);">View JSON API</a>
        | Data file: {DATA_FILE.name}
    </div>
</body>
</html>"""
    return html

# --- HTTP Handler ---

class DashboardHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path == "/" or parsed.path == "/index.html":
            data = load_data()
            local = get_local_status()
            html = generate_html(data, local)
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(html.encode())

        elif parsed.path == "/api/json":
            data = load_data()
            local = get_local_status()
            response = {
                "local_status": local,
                "latest_results": get_latest_results(data),
                "all_checks": data.get("checks", [])[-50:],
                "last_updated": data.get("last_updated", ""),
            }
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(response, indent=2, default=str).encode())

        elif parsed.path == "/api/check":
            # Trigger a check
            try:
                subprocess.run(
                    [sys.executable, str(WATCH_SCRIPT), "--source", "all"],
                    capture_output=True, text=True, timeout=120
                )
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"status": "check_completed"}).encode())
            except Exception as e:
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"error": str(e)}).encode())

        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"Not Found")

    def log_message(self, format, *args):
        pass  # Suppress default logging

# --- Main ---

def main():
    parser = argparse.ArgumentParser(
        description="xe_fan_dashboard - Web dashboard for B580 fan control monitor"
    )
    parser.add_argument("--port", type=int, default=8585,
                        help="Port to listen on (default: 8585)")
    parser.add_argument("--host", default="127.0.0.1",
                        help="Host to bind to (default: 127.0.0.1)")
    parser.add_argument("--check", action="store_true",
                        help="Run a check before starting the dashboard")
    parser.add_argument("--background-check", action="store_true",
                        help="Run periodic checks in background (every hour)")

    args = parser.parse_args()

    if args.check:
        print("Running initial check...")
        subprocess.run(
            [sys.executable, str(WATCH_SCRIPT), "--source", "all"],
            timeout=120
        )

    if args.background_check:
        print("Starting background checker (hourly)...")
        thread = threading.Thread(target=background_checker, args=(3600,), daemon=True)
        thread.start()

    server = HTTPServer((args.host, args.port), DashboardHandler)
    print(f"\n  xe_fan_dashboard running at http://{args.host}:{args.port}")
    print(f"  Open this URL in your browser to see the monitor dashboard.")
    print(f"  Press Ctrl+C to stop.\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nDashboard stopped.")
        server.server_close()

if __name__ == "__main__":
    main()
