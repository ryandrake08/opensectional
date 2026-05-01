#!/usr/bin/env python3
"""Download FAA aeronautical data for OpenSectional.

Scrapes the FAA NASR subscription page and related sources to download
the data files needed by the build_* ingesters.

Sources (each maps 1:1 to a build_* script):
    nasr  — NASR 28-day CSV subscription      (→ build_nasr)
    shp   — Class airspace shapefiles         (→ build_shp)
    aixm  — AIXM 5.0 special use airspace     (→ build_aixm)
    dof   — Digital Obstacle File             (→ build_dof)
    adiz  — ADIZ boundaries (ArcGIS)          (→ build_adiz)

TFRs were previously downloaded here too; they're now fetched and
parsed in-app at runtime via tfr_source. nasr, shp, and aixm share
the FAA subscription page and its Current/Preview cycle split;
dof and adiz are independent.

Usage:
    python3 tools/download_all.py [--preview] [--only NAMES ...] [output_dir]
"""

import argparse
import json
import os
import sys
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


SOURCES = ("nasr", "shp", "aixm", "dof", "adiz")


def main():
    parser = argparse.ArgumentParser(
        description="Download FAA aeronautical data for OpenSectional."
    )
    parser.add_argument("--preview", action="store_true",
                        help="Download Preview cycle data instead of Current")
    parser.add_argument("--only", nargs="+", choices=SOURCES, metavar="SOURCE",
                        help=f"Download only the named sources (default: all). "
                             f"Choices: {', '.join(SOURCES)}.")
    parser.add_argument("output_dir", nargs="?", default="./",
                        help="Output directory (default: current working directory)")
    args = parser.parse_args()

    wanted = set(args.only) if args.only else set(SOURCES)
    section = "Preview" if args.preview else "Current"
    output_dir = args.output_dir
    os.makedirs(output_dir, exist_ok=True)

    paths = {}

    # NASR / shp / aixm share the subscription subpage; fetch it once if
    # any of them is requested.
    if wanted & {"nasr", "shp", "aixm"}:
        print(f"Fetching NASR subscription page ({section})...")
        main_soup = fetch_html(_NASR_URL)
        subpage_soup, subpage_url = find_nasr_subpage(main_soup, section)

        if "nasr" in wanted:
            print("Finding NASR CSV link...")
            url = find_download_link(subpage_soup, subpage_url,
                                     "Comma-separated Values (CSV) Data")
            if not url:
                raise ValueError("Could not find NASR CSV link")
            print("Downloading NASR CSV data...")
            paths["nasr"] = download_file(url, output_dir)

        if "shp" in wanted:
            print("Finding class airspace shapefile link...")
            url = find_download_link(subpage_soup, subpage_url, "Shape File Data")
            if not url:
                raise ValueError("Could not find class airspace shapefile link")
            print("Downloading class airspace shapefiles...")
            paths["shp"] = download_file(url, output_dir)

        if "aixm" in wanted:
            print("Finding SUA AIXM 5.0 link...")
            url = find_download_link(subpage_soup, subpage_url, "AIXM Data",
                                     filename_contains="aixm5.0")
            if not url:
                raise ValueError("Could not find SUA AIXM 5.0 link")
            print("Downloading SUA AIXM 5.0 data...")
            paths["aixm"] = download_file(url, output_dir)

    if "dof" in wanted:
        print("Finding Digital Obstacle File link...")
        dof_url = find_dof_link()
        if not dof_url:
            raise ValueError("Could not find Digital Obstacle File link")
        print("Downloading Digital Obstacle File...")
        paths["dof"] = download_file(dof_url, output_dir)

    if "adiz" in wanted:
        paths["adiz"] = download_adiz(output_dir)

    # Rebuild hints: one line per source that was downloaded, so users who
    # ran `--only <x>` get the matching single-source build command.
    print(f"\nDownloaded to {output_dir}/")
    print("\nTo rebuild:")
    if wanted == set(SOURCES):
        print(f"  python3 tools/build_all.py "
              f"{paths['nasr']} {paths['shp']} {paths['aixm']} "
              f"{paths['dof']} {paths['adiz']} osect.db")
    else:
        for name in SOURCES:
            if name in wanted:
                print(f"  python3 tools/build_{name}.py {paths[name]} osect.db")
        print("  python3 tools/build_search.py osect.db")


if __name__ == "__main__":
    main()
