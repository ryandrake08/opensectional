#!/usr/bin/env python3
"""Download FAA aeronautical data for NASRBrowse.

Scrapes the FAA NASR subscription page and related sources to download
all data files needed by build_nasr_db.py. Uses conditional requests
(If-Modified-Since) to avoid re-downloading unchanged files.

Sources:
    - NASR 28-day CSV subscription
    - Class airspace shapefiles
    - AIXM 5.0 special use airspace
    - Digital Obstacle File (DOF)
    - ADIZ boundaries (ArcGIS FeatureServer)
    - TFR XNOTAM data (tfr.faa.gov)

Usage:
    python3 tools/download_nasr_data.py [--current|--preview] [--output-dir DIR]
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

try:
    import bs4
except ImportError:
    print("Error: beautifulsoup4 is required. Install with: pip install beautifulsoup4",
          file=sys.stderr)
    sys.exit(1)


_NASR_URL = "https://www.faa.gov/air_traffic/flight_info/aeronav/aero_data/NASR_Subscription/"
_DOF_URL = "https://www.faa.gov/air_traffic/flight_info/aeronav/digital_products/dof/"
_ADIZ_URL = (
    "https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/ArcGIS/rest/services/"
    "Airspace/FeatureServer/0/query"
    "?where=TYPE_CODE%3D%27ADIZ%27&outFields=*&outSR=4326&f=geojson"
)
_TFR_LIST_URL = "https://tfr.faa.gov/tfrapi/getTfrList"
_TFR_XML_URL = "https://tfr.faa.gov/download/detail_{notam_id}.xml"


def fetch_html(url):
    """Fetch a URL and return parsed BeautifulSoup document."""
    with urllib.request.urlopen(url) as response:
        html = response.read().decode("utf-8")
    return bs4.BeautifulSoup(html, "html.parser")


def find_nasr_subpage(soup, section_name):
    """Find and follow the link in a NASR page section (Current or Preview).

    Returns the parsed BeautifulSoup of the subpage and its base URL.
    """
    article = soup.find("article", id="content")
    if article is None:
        raise ValueError("NASR page article content not found")

    section_h2 = article.find("h2", string=section_name)
    if section_h2 is None:
        raise ValueError(f"'{section_name}' section not found on NASR page")

    section_ul = section_h2.find_next("ul")
    if section_ul is None:
        raise ValueError(f"No list found under '{section_name}' section")

    link = section_ul.find("a")
    if link is None:
        raise ValueError(f"No link found in '{section_name}' section")

    subpage_url = urllib.parse.urljoin(_NASR_URL, str(link["href"]))
    return fetch_html(subpage_url), subpage_url


def find_download_link(soup, base_url, section_text, filename_contains=None):
    """Find a download link under a section header on a NASR subpage.

    Searches for an h2/h3/h4 containing section_text, then finds a
    .zip link. If filename_contains is set, only matches links whose
    URL contains that substring.
    """
    for tag in soup.find_all(["h2", "h3", "h4"]):
        if section_text.lower() in tag.get_text().lower():
            for sibling in tag.find_next_siblings():
                if sibling.name in ["h2", "h3", "h4"]:
                    break
                for a in sibling.find_all("a", href=True):
                    href = str(a["href"])
                    if not href.endswith(".zip"):
                        continue
                    if filename_contains and filename_contains not in href:
                        continue
                    return urllib.parse.urljoin(base_url, href)
    return None


def find_dof_link():
    """Scrape the DOF page for the download ZIP link."""
    soup = fetch_html(_DOF_URL)
    article = soup.find("article", id="content")
    if article is None:
        raise ValueError("DOF page article content not found")

    for a in article.find_all("a", href=True):
        href = str(a["href"])
        if href.endswith(".zip") and "dof" in href.lower():
            return urllib.parse.urljoin(_DOF_URL, href)

    raise ValueError("Could not find DOF ZIP link on the DOF page")


def download_file(url, output_dir, filename=None):
    """Download a file from a URL into output_dir."""
    if filename is None:
        filename = os.path.basename(urllib.parse.urlparse(url).path)
    if not filename:
        filename = "download"

    filepath = os.path.join(output_dir, filename)

    with urllib.request.urlopen(url) as response:
        data = response.read()
        with open(filepath, "wb") as f:
            f.write(data)
        size_mb = len(data) / (1024 * 1024)
        print(f"  Downloaded {filename} ({size_mb:.1f} MB)")

    return filepath


def download_adiz(output_dir):
    """Download ADIZ boundary data from ArcGIS FeatureServer."""
    filepath = os.path.join(output_dir, "adiz.geojson")

    print("Downloading ADIZ boundaries from ArcGIS...")
    with urllib.request.urlopen(_ADIZ_URL) as response:
        data = response.read()

    # Validate it's valid GeoJSON with features
    geojson = json.loads(data)
    count = len(geojson.get("features", []))
    if count == 0:
        raise ValueError("ADIZ query returned no features")

    with open(filepath, "wb") as f:
        f.write(data)

    print(f"  Downloaded adiz.geojson ({count} features)")
    return filepath


def download_tfrs(output_dir):
    """Download TFR XNOTAM XML files from tfr.faa.gov.

    Fetches the active TFR list, then downloads each TFR's detail XML
    into a tfr_data/ subdirectory.  Returns the path to that directory.
    """
    tfr_dir = os.path.join(output_dir, "tfr_data")
    os.makedirs(tfr_dir, exist_ok=True)

    print("Fetching TFR list from tfr.faa.gov...")
    req = urllib.request.Request(_TFR_LIST_URL)
    req.add_header("Accept", "application/json")
    with urllib.request.urlopen(req) as response:
        tfr_list = json.loads(response.read().decode("utf-8"))

    if not tfr_list:
        print("  No active TFRs found")
        return tfr_dir

    print(f"  {len(tfr_list)} active TFRs, downloading XNOTAM XML...")
    downloaded = 0
    for tfr in tfr_list:
        notam_id = tfr.get("notam_id") or tfr.get("gid", "")
        if not notam_id:
            continue
        # URL uses underscore in place of slash: "6/4033" -> "6_4033"
        url_id = notam_id.replace("/", "_")
        filename = f"detail_{url_id}.xml"
        url = _TFR_XML_URL.format(notam_id=url_id)
        try:
            with urllib.request.urlopen(url) as resp:
                data = resp.read()
            with open(os.path.join(tfr_dir, filename), "wb") as f:
                f.write(data)
            downloaded += 1
        except urllib.error.HTTPError as e:
            print(f"  Warning: failed to download {notam_id}: HTTP {e.code}")

    print(f"  Downloaded {downloaded} TFR XML files to {tfr_dir}/")
    return tfr_dir


def main():
    parser = argparse.ArgumentParser(
        description="Download FAA aeronautical data for NASRBrowse."
    )
    parser.add_argument("--preview", action="store_true",
                        help="Download Preview cycle data instead of Current")
    parser.add_argument("output_dir", nargs="?", default="./",
                        help="Output directory (default: current working directory)")
    args = parser.parse_args()

    section = "Preview" if args.preview else "Current"
    output_dir = args.output_dir
    os.makedirs(output_dir, exist_ok=True)

    # Scrape NASR subscription page
    print(f"Fetching NASR subscription page ({section})...")
    main_soup = fetch_html(_NASR_URL)
    subpage_soup, subpage_url = find_nasr_subpage(main_soup, section)

    # Find download links on the subpage
    csv_url = find_download_link(subpage_soup, subpage_url,
                                 "Comma-separated Values (CSV) Data")
    shp_url = find_download_link(subpage_soup, subpage_url,
                                 "Shape File Data")
    aixm_url = find_download_link(subpage_soup, subpage_url,
                                  "AIXM Data", filename_contains="aixm5.0")

    if not csv_url:
        raise ValueError("Could not find CSV download link")
    if not shp_url:
        raise ValueError("Could not find shapefile download link")
    if not aixm_url:
        raise ValueError("Could not find AIXM download link")

    print("\nDownloading NASR CSV data...")
    csv_path = download_file(csv_url, output_dir)

    print("Downloading class airspace shapefiles...")
    shp_path = download_file(shp_url, output_dir)

    print("Downloading AIXM 5.0 SUA data...")
    aixm_path = download_file(aixm_url, output_dir)

    # Download DOF
    print("Finding DOF download link...")
    dof_url = find_dof_link()
    print("Downloading Digital Obstacle File...")
    dof_path = download_file(dof_url, output_dir)

    # Download ADIZ
    adiz_path = download_adiz(output_dir)

    # Download TFRs
    tfr_dir = download_tfrs(output_dir)

    # Print summary
    print(f"\nAll data downloaded to {output_dir}/")
    print("\nTo build the database, run:")
    print(f"  python3 tools/build_nasr_db.py {csv_path} {shp_path} {aixm_path} {dof_path} {adiz_path} {tfr_dir} nasr.db")


if __name__ == "__main__":
    main()
