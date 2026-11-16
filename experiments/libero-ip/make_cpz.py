#!/usr/bin/env python3
"""Build pfdaq_demo_regs-1.0.0.cpz: the register core as a package for Libero's 'Add Core to Vault'.

The layout copies a plain (unprotected) vendor core from the vault: index_entry.xml describes the
package, core_xml.zip holds the core description (ports, parameters, bus interfaces), and
other_filesets.zip holds the Verilog. Nothing here is a documented Libero format; it was written
by reading the vault and the project's own core description (module F07).

usage: make_cpz.py [out_dir]
       make_cpz.py out_dir --repack VAULT LIBRARY NAME VERSION
The second form packs a core that is already in a vault (for example CoreAPB3 in the MegaVault) into a .cpz,
so that it can be added to a scratch vault the same way as the register core.
"""
import hashlib, io, os, sys, zipfile

VENDOR, LIBRARY, NAME, VERSION = 'pfdaq', 'Demo', 'pfdaq_demo_regs', '1.0.0'
HERE = os.path.dirname(os.path.abspath(__file__))
VSRC = 'pfdaq_demo_regs.v'
FSID = 'Source-Verilog-ALL'


def zipbytes(files):
    b = io.BytesIO()
    with zipfile.ZipFile(b, 'w', zipfile.ZIP_DEFLATED) as z:
        for n, d in files:
            z.writestr(n, d)
    return b.getvalue()


def bif(name, role, bus, lib, bus_ver, pairs):
    sm = ''.join('      <spirit:signalName>\n        <spirit:componentSignalName>%s</spirit:componentSignalName>'
                 '<spirit:busSignalName>%s</spirit:busSignalName></spirit:signalName>\n' % p for p in pairs)
    return ('  <spirit:busInterface>\n    <spirit:name>%s</spirit:name>\n'
            '    <spirit:busType spirit:vendor="AMBA" spirit:library="%s" spirit:name="%s" spirit:version="%s"/>\n'
            '    <spirit:%s></spirit:%s>\n    <spirit:signalMap>\n%s    </spirit:signalMap>\n'
            '    <spirit:vendorExtensions></spirit:vendorExtensions>\n  </spirit:busInterface>\n') % (
            name, lib, bus, bus_ver, role, role, sm)


def sig(n, d, w=None):
    r = '        <spirit:left>%d</spirit:left>\n        <spirit:right>0</spirit:right>\n' % (w - 1) if w else ''
    return '      <spirit:signal>\n        <spirit:name>%s</spirit:name>\n        <spirit:direction>%s</spirit:direction>\n%s      </spirit:signal>\n' % (n, d, r)


def core_xml():
    apb = [(s, s) for s in ('PADDR', 'PENABLE', 'PWRITE', 'PRDATA', 'PWDATA', 'PREADY', 'PSLVERR')] + [('PSEL', 'PSELx')]
    axs = [(s, s) for s in ('TVALID', 'TREADY', 'TDATA', 'TLAST')]
    sigs = ''.join([sig('PCLK', 'in'), sig('PRESETN', 'in'), sig('PSEL', 'in'), sig('PENABLE', 'in'), sig('PWRITE', 'in'),
                    sig('PADDR', 'in', 32), sig('PWDATA', 'in', 32), sig('PRDATA', 'out', 32), sig('PREADY', 'out'),
                    sig('PSLVERR', 'out'), sig('TDATA', 'out', 32), sig('TVALID', 'out'), sig('TREADY', 'in'), sig('TLAST', 'out')])
    return '''<spirit:component xmlns:actel-cc="http://www.actel.com/XMLSchema/CoreConsole" xmlns:spirit="http://www.spiritconsortium.org/XMLSchema/SPIRIT/1.1" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="http://www.spiritconsortium.org/XMLSchema/SPIRIT/1.1 http://www.spiritconsortium.org/XMLSchema/SPIRIT/1.1/component.xsd">
  <spirit:vendor>%(v)s</spirit:vendor>
  <spirit:library>%(l)s</spirit:library>
  <spirit:name>%(n)s</spirit:name>
  <spirit:version>%(ver)s</spirit:version>
  <spirit:busInterfaces>
%(bifs)s  </spirit:busInterfaces>
  <spirit:memoryMaps>
  </spirit:memoryMaps>
  <spirit:hwModel>
    <spirit:views>
      <spirit:view>
        <spirit:name>VerilogSource</spirit:name>
        <spirit:envIdentifier></spirit:envIdentifier>
        <spirit:language>Verilog</spirit:language>
        <spirit:fileSetRef>%(fs)s</spirit:fileSetRef>
        <spirit:vendorExtensions>
          <actel-cc:fsInclusionRules>
            <actel-cc:fsInclusionRule spirit:fileSetRef="%(fs)s"></actel-cc:fsInclusionRule></actel-cc:fsInclusionRules></spirit:vendorExtensions>
      </spirit:view>
    </spirit:views>
    <spirit:signals>
%(sigs)s    </spirit:signals>
    <spirit:hwParameters>
      <spirit:hwParameter spirit:id="DEFAULT_LEN" spirit:name="DEFAULT_LEN" spirit:prompt="DEFAULT_LEN:" spirit:dataType="int" spirit:resolve="user">4</spirit:hwParameter>
    </spirit:hwParameters>
  </spirit:hwModel>
  <spirit:choices>
    <spirit:choice>
      <spirit:name>Choice_FamilyValue</spirit:name>
      <spirit:enumeration spirit:text="PolarFire SoC">27</spirit:enumeration>
    </spirit:choice>
  </spirit:choices>
  <spirit:fileSets>
  <spirit:fileSet spirit:fileSetId="%(fs)s">
    <spirit:file>
      <spirit:name>rtl/vlog/core/%(src)s</spirit:name>
      <spirit:fileType>verilogSource</spirit:fileType>
    <spirit:vendorExtensions></spirit:vendorExtensions>
    </spirit:file>
  <spirit:vendorExtensions></spirit:vendorExtensions>
  </spirit:fileSet>
  </spirit:fileSets>
  <spirit:vendorExtensions>
    <actel-cc:displayName>%(n)s</actel-cc:displayName>
    <actel-cc:type typeName="IP"/>
    <actel-cc:instantiateOnCreation value="false"/>
    <actel-cc:diveInPermission value="READ_WRITE"/>
    <actel-cc:categories categoryName="OS" style="hidden">
      <actel-cc:category name="PC">
      </actel-cc:category>
    </actel-cc:categories>
  </spirit:vendorExtensions>
</spirit:component>
''' % dict(v=VENDOR, l=LIBRARY, n=NAME, ver=VERSION, fs=FSID, src=VSRC, sigs=sigs,
           bifs=bif('APB_TARGET', 'slave', 'APB', 'AMBA2', 'r0p0', apb) + bif('AXIS_INITIATOR', 'master', 'AXI4Stream', 'AMBA4', 'r0p0_1', axs))


def main(out):
    src = open(os.path.join(HERE, VSRC), 'rb').read()
    fs_zip = zipbytes([('rtl/vlog/core/' + VSRC, src)])
    other = zipbytes([(FSID + '.zip', fs_zip)])
    cx = zipbytes([('core_xml_fileset.zip', zipbytes([(NAME + '.xml', core_xml())]))])
    md5 = lambda b: hashlib.md5(b).hexdigest()
    idx = '''<?xml version="1.0" encoding="UTF-8" standalone="no" ?><core_mlv xmlns="http://actel.com/sweng/afi" index_data_id="2" index_format_id="3"><vendor>%(v)s</vendor><library>%(l)s</library><name>%(n)s</name><version>%(ver)s</version><description>APB slave with five registers and an AXI4-Stream initiator that sends a burst of words. Example core of module F07.</description><display_name>%(n)s</display_name><package_timestamp>20261115000000</package_timestamp><core_type>IP</core_type><devices><family name="PolarFireSoC"/></devices><core_packages><core_package><id>core_xml</id><download_options><location>core_xml.zip</location><signature>%(m1)s</signature><size>%(s1)d</size></download_options><extract_options><extract_on_add>true</extract_on_add></extract_options><fileset_packages><fileset_package><id>core_xml_fileset</id><file_name>core_xml_fileset.zip</file_name><extract_options/></fileset_package></fileset_packages></core_package><core_package><id>other_filesets</id><download_options><location>other_filesets.zip</location><signature>%(m2)s</signature><size>%(s2)d</size></download_options><extract_options/><fileset_packages><fileset_package><id>%(fs)s</id><file_name>%(fs)s.zip</file_name><extract_options/></fileset_package></fileset_packages></core_package></core_packages><core_config_file name="%(n)s.xml" package_name="core_xml"/><filter_categories filter_name="OS"><category name="PC"/><category name="Linux"/></filter_categories><filter_categories filter_name="min_libero_version"><category name="8.6"/></filter_categories><filter_categories filter_name="Product Family"><category name="PolarFireSoC"/></filter_categories><group name="User Defined"/></core_mlv>
''' % dict(v=VENDOR, l=LIBRARY, n=NAME, ver=VERSION, fs=FSID, m1=md5(cx), s1=len(cx), m2=md5(other), s2=len(other))
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, '%s-%s.cpz' % (NAME, VERSION))
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('index_entry.xml', idx); z.writestr('core_xml.zip', cx); z.writestr('other_filesets.zip', other)
    print(path)


def repack(out, vault, library, name, version):
    import re
    idx = open(os.path.join(vault, 'index.xml'), encoding='utf-8').read()
    pat = (r'<core_mlv [^>]*>(?:(?!</core_mlv>).)*?<library>%s</library><name>%s</name><version>%s</version>.*?</core_mlv>'
           % (re.escape(library), re.escape(name), re.escape(version)))
    m = re.search(pat, idx, re.S)
    if not m:
        sys.exit('core not found in %s/index.xml' % vault)
    entry = m.group(0)
    if 'xmlns=' not in entry[:200]:
        entry = entry.replace('<core_mlv ', '<core_mlv xmlns="http://actel.com/sweng/afi" ', 1)
    pkg = os.path.join(vault, 'Components', 'Actel', library, name, version, 'pkg')
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, '%s-%s.cpz' % (name, version))
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as z:
        z.writestr('index_entry.xml', '<?xml version="1.0" encoding="UTF-8" standalone="no" ?>' + entry)
        for f in sorted(os.listdir(pkg)):
            z.write(os.path.join(pkg, f), f)
    print(path)


if __name__ == '__main__':
    a = sys.argv[1:]
    if '--repack' in a:
        i = a.index('--repack')
        repack(a[0] if i else '.', *a[i + 1:i + 5])
    else:
        main(a[0] if a else '.')
