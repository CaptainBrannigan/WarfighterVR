// Fb2Dump: reads Medal of Honor Warfighter's Frostbite 2 data (Data\cas.cat + Data\Win32\*.sb), BF3 layout.
// Single file, no dependencies: java tools/fb2dump/Fb2Dump.java <command> ...
//
//   list  <data dir> <sb name>                 every bundle in the superbundle and its ebx names
//   ebx   <data dir> <sb name> <filter> <out>  every ebx whose name contains <filter>, as text, into <out>
//   find  <data dir> <sb name> <type> <out>    every ebx holding an instance of <type> (e.g. SkeletonAsset), as text
//
// <sb name> is relative to Data\Win32 without the extension (Weapons, MOHW/Levels/..., UI). Formats, from the BF3 tools
// of the time (Frankelstner's dumpers): bjson (the sb's typed key-value tree), cas.cat (sha1 -> cas file, offset, size),
// cas blocks (8-byte big-endian header, zlib or stored), ebx (reflected instances: field and complex descriptors keyed by
// a 33-multiplier name hash, then the payload).
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.zip.Inflater;

public class Fb2Dump {

    // ---------------------------------------------------------------- bjson
    static final class Bj {
        final ByteBuffer b;
        Bj(byte[] d) { b = ByteBuffer.wrap(d).order(ByteOrder.LITTLE_ENDIAN); }
        long leb() { long v = 0; int s = 0; while (true) { int x = b.get() & 0xFF; v |= (long) (x & 0x7F) << s; if ((x & 0x80) == 0) return v; s += 7; } }
        String cstr() { StringBuilder sb = new StringBuilder(); byte c; while ((c = b.get()) != 0) sb.append((char) (c & 0xFF)); return sb.toString(); }
        // One entry: type byte, name (unless the 0x80 flag), value.
        Object value(int type) {
            switch (type & 0x1F) {
                case 0x01: { long len = leb(); int end = b.position() + (int) len; List<Object> l = new ArrayList<>(); while (b.position() < end) { int t = b.get() & 0xFF; if (t == 0) break; if ((t & 0x80) == 0) cstr(); l.add(value(t)); } b.position(end); return l; }
                case 0x02: { long len = leb(); int end = b.position() + (int) len; Map<String, Object> m = new LinkedHashMap<>(); while (b.position() < end) { int t = b.get() & 0xFF; if (t == 0) break; String n = (t & 0x80) == 0 ? cstr() : ""; m.put(n, value(t)); } b.position(end); return m; }
                case 0x06: return b.get() != 0;
                case 0x07: { int len = (int) leb(); byte[] s = new byte[len]; b.get(s); int n = len; while (n > 0 && s[n - 1] == 0) n--; return new String(s, 0, n, StandardCharsets.ISO_8859_1); }
                case 0x08: return b.getInt();
                case 0x09: return b.getLong();
                case 0x0F: { byte[] g = new byte[16]; b.get(g); return new Guid(g); }
                case 0x10: { byte[] h = new byte[20]; b.get(h); return new Sha(h); }
                case 0x13: { int len = (int) leb(); byte[] d = new byte[len]; b.get(d); return d; }
                default: throw new IllegalStateException("bjson type " + Integer.toHexString(type) + " at " + b.position());
            }
        }
        Object root() { int t = b.get() & 0xFF; if ((t & 0x80) == 0) cstr(); return value(t); }
    }
    record Guid(byte[] g) { public String toString() { return hex(g); } }
    record Sha(byte[] h) { public String toString() { return hex(h); } }
    static String hex(byte[] d) { StringBuilder sb = new StringBuilder(); for (byte x : d) sb.append(String.format("%02x", x)); return sb.toString(); }

    // ---------------------------------------------------------------- cas
    record CasEntry(long offset, int size, int cas) {}
    static Map<String, CasEntry> cat = new HashMap<>();
    static Path dataDir;

    static void loadCat() throws IOException {
        byte[] d = Files.readAllBytes(dataDir.resolve("cas.cat"));
        ByteBuffer b = ByteBuffer.wrap(d).order(ByteOrder.LITTLE_ENDIAN);
        b.position(16);
        byte[] h = new byte[20];
        while (b.remaining() >= 32) {
            b.get(h);
            long off = b.getInt() & 0xFFFFFFFFL; int size = b.getInt(); int cas = b.getInt();
            cat.put(hex(h), new CasEntry(off, size, cas));
        }
    }

    static byte[] readCas(String sha) throws Exception {
        CasEntry e = cat.get(sha);
        if (e == null) return null;
        byte[] raw = new byte[e.size];
        try (RandomAccessFile f = new RandomAccessFile(dataDir.resolve(String.format("cas_%02d.cas", e.cas)).toFile(), "r")) {
            f.seek(e.offset); f.readFully(raw);
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        ByteBuffer b = ByteBuffer.wrap(raw).order(ByteOrder.BIG_ENDIAN);
        // MOHW's blocks: uncompressed size, then a word whose low 24 bits are the stored size (BF3's type/size split
        // doesn't fit: live, 00000AA0 000004E2 then a zlib stream). zlib is told by its own header; anything else is stored.
        while (b.remaining() >= 8) {
            int usize = b.getInt(); int word = b.getInt(); int csize = word & 0xFFFFFF;
            if (csize == 0) csize = usize;
            if (csize > b.remaining()) throw new IllegalStateException("cas block runs past the entry");
            int p = b.position();
            boolean zlib = csize != usize && (b.get(p) & 0xFF) == 0x78;
            byte[] c = new byte[csize]; b.get(c);
            if (zlib) {
                Inflater inf = new Inflater(); inf.setInput(c); byte[] u = new byte[usize]; int n = 0; while (n < usize && !inf.finished()) { int k = inf.inflate(u, n, usize - n); if (k == 0 && inf.needsInput()) break; n += k; } inf.end();
                out.write(u, 0, n);
            } else {
                out.write(c);
            }
        }
        return out.toByteArray();
    }

    // ---------------------------------------------------------------- superbundle
    record EbxRef(String bundle, String name, String sha) {}

    @SuppressWarnings("unchecked")
    static List<EbxRef> readSb(String sbName) throws Exception {
        Path p = dataDir.resolve("Win32").resolve(sbName + ".sb");
        Object root = new Bj(Files.readAllBytes(p)).root();
        List<EbxRef> out = new ArrayList<>();
        for (Object bo : (List<Object>) ((Map<String, Object>) root).get("bundles")) {
            Map<String, Object> bundle = (Map<String, Object>) bo;
            String path = String.valueOf(bundle.get("path"));
            Object ebx = bundle.get("ebx");
            if (ebx == null) continue;
            for (Object eo : (List<Object>) ebx) {
                Map<String, Object> e = (Map<String, Object>) eo;
                out.add(new EbxRef(path, String.valueOf(e.get("name")), String.valueOf(e.get("sha1"))));
            }
        }
        return out;
    }

    // ---------------------------------------------------------------- ebx
    static int hash(String s) { int h = 5381; for (byte c : s.getBytes(StandardCharsets.ISO_8859_1)) h = (h * 33) ^ (c & 0xFF); return h; }

    static final class Ebx {
        record Field(String name, int type, int ref, int offset) { int kind() { return (type >> 4) & 0x1F; } }
        record Complex(String name, int fieldStart, int numField, int align, int type, int size) {}
        record Repeater(int complex, int count) {}
        record Array(int offset, int count, int complex) {}
        final ByteBuffer b;
        final Map<Integer, String> keys = new HashMap<>();
        final List<Field> fields = new ArrayList<>();
        final List<Complex> complexes = new ArrayList<>();
        final List<Repeater> repeaters = new ArrayList<>();
        final List<Array> arrays = new ArrayList<>();
        final List<String> externals = new ArrayList<>();
        int stringStart, payloadStart, arrayStart, guidRepeaters;
        final List<String> instanceNames = new ArrayList<>(); // type of each instance, in order
        final List<String> instanceGuids = new ArrayList<>();
        final List<Integer> instanceOffsets = new ArrayList<>();
        final List<Integer> instanceComplex = new ArrayList<>();

        Ebx(byte[] d) {
            b = ByteBuffer.wrap(d).order(ByteOrder.LITTLE_ENDIAN);
            int magic = b.getInt();
            if (magic != 0x0FB2D1CE) throw new IllegalStateException("not ebx: " + Integer.toHexString(magic));
            // BF3's header: eleven 32-bit values (live: 2496 224 1 0 2 21 63 976 48 2 160 for a 2720-byte file), then the
            // file's GUID and its primary instance's GUID. Every instance carries its own GUID.
            int absString = b.getInt(); b.getInt(); int numGuid = b.getInt(); b.getInt();
            int numInstRep = b.getInt(); int numComplex = b.getInt(); int numField = b.getInt(); int lenName = b.getInt();
            int lenString = b.getInt(); int numArray = b.getInt(); int lenPayload = b.getInt();
            guidRepeaters = Integer.MAX_VALUE;
            byte[] g = new byte[16]; b.get(g); b.get(g); // file guid, primary instance guid
            for (int i = 0; i < numGuid; i++) { byte[] f = new byte[16], ins = new byte[16]; b.get(f); b.get(ins); externals.add(hex(f) + "/" + hex(ins)); }
            byte[] names = new byte[lenName]; b.get(names);
            int s = 0;
            for (int i = 0; i <= lenName; i++) if (i == lenName || names[i] == 0) { if (i > s) { String k = new String(names, s, i - s, StandardCharsets.ISO_8859_1); keys.put(hash(k), k); } s = i + 1; }
            for (int i = 0; i < numField; i++) { int h = b.getInt(); int t = b.getShort() & 0xFFFF; int r = b.getShort() & 0xFFFF; int o = b.getInt(); b.getInt(); fields.add(new Field(keys.getOrDefault(h, "?" + Integer.toHexString(h)), t, r, o)); }
            for (int i = 0; i < numComplex; i++) { int h = b.getInt(); int fs = b.getInt(); int nf = b.get() & 0xFF; int al = b.get() & 0xFF; int t = b.getShort() & 0xFFFF; int sz = b.getShort() & 0xFFFF; b.getShort(); complexes.add(new Complex(keys.getOrDefault(h, "?" + Integer.toHexString(h)), fs, nf, al == 0 ? 1 : al, t, sz)); }
            for (int i = 0; i < numInstRep; i++) { b.getInt(); int rep = b.getInt(); int ci = b.getInt(); repeaters.add(new Repeater(ci, rep)); }
            while (b.position() % 16 != 0) b.get();
            for (int i = 0; i < numArray; i++) { int o = b.getInt(); int c = b.getInt(); int ci = b.getInt(); arrays.add(new Array(o, c, ci)); }
            stringStart = absString;
            payloadStart = absString + lenString;
            arrayStart = payloadStart + lenPayload;
            b.position(payloadStart);
            int nonGuid = 0;
            for (int r = 0; r < repeaters.size(); r++) {
                Repeater rep = repeaters.get(r);
                Complex c = complexes.get(rep.complex);
                for (int k = 0; k < rep.count; k++) {
                    // no padding before an instance: they're packed, whatever the type's alignment (2026-11-22)
                    String guid;
                    if (r < guidRepeaters) { byte[] ig = new byte[16]; b.get(ig); guid = hex(ig); } else guid = "#" + (nonGuid++);
                    instanceNames.add(c.name); instanceGuids.add(guid); instanceOffsets.add(b.position()); instanceComplex.add(rep.complex);
                    b.position(b.position() + c.size);
                }
            }
        }

        String str(int off) {
            if (off < 0) return "null";
            int p = stringStart + off; StringBuilder sb = new StringBuilder();
            while (p < b.limit() && b.get(p) != 0) sb.append((char) (b.get(p++) & 0xFF));
            return sb.toString();
        }

        void dump(StringBuilder out) {
            for (int i = 0; i < instanceNames.size(); i++) {
                out.append(instanceNames.get(i)).append(' ').append(instanceGuids.get(i)).append('\n');
                complex(out, instanceComplex.get(i), instanceOffsets.get(i), 1);
            }
        }

        void indent(StringBuilder out, int depth) { for (int i = 0; i < depth; i++) out.append("    "); }

        void complex(StringBuilder out, int ci, int base, int depth) {
            Complex c = complexes.get(ci);
            for (int f = c.fieldStart; f < c.fieldStart + c.numField; f++) {
                Field fd = fields.get(f);
                if (fd.kind() == 0) { // the base class's fields, inlined
                    complex(out, fd.ref, base + fd.offset, depth);
                    continue;
                }
                field(out, fd, base + fd.offset, depth);
            }
        }

        void field(StringBuilder out, Field fd, int pos, int depth) {
            indent(out, depth);
            out.append(fd.name).append(' ');
            switch (fd.kind()) {
                case 2: out.append("::").append(complexes.get(fd.ref).name).append('\n'); complex(out, fd.ref, pos, depth + 1); return;
                case 3: {
                    int v = b.getInt(pos);
                    if (v == 0) out.append("*null");
                    else if ((v & 0x80000000) != 0) { int e = v & 0x7FFFFFFF; out.append("*ext ").append(e < externals.size() ? externals.get(e) : "<import " + e + ">"); }
                    else if (v - 1 < instanceNames.size()) out.append("*").append(instanceNames.get(v - 1)).append(' ').append(instanceGuids.get(v - 1));
                    else out.append("<instance ").append(v).append('>');
                    break;
                }
                case 4: {
                    int index = b.getInt(pos);
                    if (index < 0 || index >= arrays.size()) { out.append("<array ").append(index).append(">\n"); return; }
                    Array a = arrays.get(index); Complex ac = complexes.get(fd.ref); Field member = fields.get(ac.fieldStart);
                    out.append("::").append(ac.name).append(" [").append(a.count).append("]\n");
                    int esize = elementSize(member);
                    for (int i = 0; i < a.count && i < 4096; i++) field(out, new Field("member(" + i + ")", member.type, member.ref, 0), arrayStart + a.offset + i * esize, depth + 1);
                    return;
                }
                case 7: case 9: out.append(str(b.getInt(pos))); break;
                case 8: out.append(b.getInt(pos)); break;
                case 0xA: out.append(b.get(pos) != 0); break;
                case 0xB: out.append(b.get(pos)); break;
                case 0xC: out.append(b.get(pos) & 0xFF); break;
                case 0xD: out.append(b.getShort(pos)); break;
                case 0xE: out.append(b.getShort(pos) & 0xFFFF); break;
                case 0xF: out.append(b.getInt(pos)); break;
                case 0x10: out.append(b.getInt(pos) & 0xFFFFFFFFL); break;
                case 0x11: case 0x12: out.append(b.getLong(pos)); break;
                case 0x13: out.append(b.getFloat(pos)); break;
                case 0x14: out.append(b.getDouble(pos)); break;
                case 0x15: { byte[] g = new byte[16]; for (int i = 0; i < 16; i++) g[i] = b.get(pos + i); out.append(hex(g)); break; }
                case 0x16: { byte[] g = new byte[20]; for (int i = 0; i < 20; i++) g[i] = b.get(pos + i); out.append(hex(g)); break; }
                default: out.append("<type ").append(Integer.toHexString(fd.type)).append('>');
            }
            out.append('\n');
        }

        int elementSize(Field member) {
            switch (member.kind()) {
                case 2: return complexes.get(member.ref).size;
                case 0xA: case 0xB: case 0xC: return 1;
                case 0xD: case 0xE: return 2;
                case 0x11: case 0x12: case 0x14: return 8;
                case 0x15: return 16;
                case 0x16: return 20;
                default: return 4;
            }
        }
    }

    // ---------------------------------------------------------------- commands
    public static void main(String[] a) throws Exception {
        if (a.length < 3) { System.err.println("list|ebx|find <data dir> <sb name> ..."); return; }
        dataDir = Paths.get(a[1]);
        loadCat();
        List<EbxRef> refs = readSb(a[2]);
        switch (a[0]) {
            case "list":
                for (EbxRef r : refs) System.out.println(r.bundle + "\t" + r.name);
                break;
            case "ebx": case "find": {
                Path out = Paths.get(a[4]); Files.createDirectories(out);
                Set<String> done = new HashSet<>(); int n = 0, bad = 0;
                for (EbxRef r : refs) {
                    if (!done.add(r.name)) continue;
                    if (a[0].equals("ebx") && !r.name.toLowerCase().contains(a[3].toLowerCase())) continue;
                    try {
                        byte[] d = readCas(r.sha);
                        if (d == null) { bad++; continue; }
                        Ebx e = new Ebx(d);
                        if (a[0].equals("find") && !e.instanceNames.contains(a[3])) continue;
                        StringBuilder sb = new StringBuilder();
                        e.dump(sb);
                        Path f = out.resolve(r.name.replace('/', '_') + ".txt");
                        Files.writeString(f, sb.toString(), StandardCharsets.ISO_8859_1);
                        n++;
                    } catch (Exception ex) { bad++; System.err.println(r.name + ": " + ex); }
                }
                System.out.println(n + " written, " + bad + " failed");
                break;
            }
            default: System.err.println("unknown command");
        }
    }
}
