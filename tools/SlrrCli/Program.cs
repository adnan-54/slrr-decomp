using System;
using System.IO;
using System.Linq;
using SlrrLib.Model;

// slrrcli dump <file.rpk>                 header, external references and every RES entry (SlrrLib's reading)
// slrrcli rpk2rdb2 <in.rpk> <out.rdb2>    SlrrLib text form (<RPK>/<RES>/<RSD> blocks)
// slrrcli rdb22rpk <in.rdb2> <out.rpk>    back to binary
// slrrcli roundtrip <in.rpk>              rpk -> rdb2 -> rpk, compare bytes with the original
static class P
{
    static int Main(string[] a)
    {
        if (a.Length < 2) { Console.Error.WriteLine("usage: slrrcli dump|rpk2rdb2|rdb22rpk|roundtrip ..."); return 2; }
        switch (a[0])
        {
            case "dump":
            {
                var r = new BinaryRpk(a[1], true);
                Console.WriteLine($"sig={r.FileHeader.Siganture} ver={r.FileHeader.Version512:x} extRefs={r.FileHeader.ExternalReferencesCount} "
                                  + $"entriesSize={r.EntriesHeader.EntriesSize:x} entries={r.EntriesHeader.EntriesCount} type1={r.EntriesHeader.EntriesType1Count} nonType1={r.EntriesHeader.EntriesNonType1Count}");
                foreach (var e in r.ExternalReferences) Console.WriteLine($"ext[{e.IndexOfReference}] {e.ReferenceString}");
                foreach (var e in r.RESEntries)
                    Console.WriteLine($"res super={e.SuperID:x8} id={e.TypeID:x8} type={e.TypeOfEntry} add={e.AdditionalType} parentCompat={e.IsParentCompatible} rsd@{e.FileOffsetOfRSD:x}+{e.RSDLength:x} alias={e.Alias}");
                return 0;
            }
            case "rpk2rdb2":
            {
                var tmp = Path.GetTempFileName(); File.Copy(a[1], tmp, true);
                new RpkExchange().SaveRpkToRDB2(tmp);
                File.Move(tmp + ".RDB2", a[2], true); File.Delete(tmp);
                return 0;
            }
            case "rdb22rpk":
            {
                var rpk = new RpkExchange().LoadRPKFromRDB2(a[1]);
                using var fs = File.Create(a[2]); rpk.SaveAs(fs);
                return 0;
            }
            case "roundtrip":
            {
                var tmp = Path.GetTempFileName(); File.Copy(a[1], tmp, true);
                new RpkExchange().SaveRpkToRDB2(tmp);
                var rpk = new RpkExchange().LoadRPKFromRDB2(tmp + ".RDB2");
                var ms = new MemoryStream(); rpk.SaveAs(ms);
                var orig = File.ReadAllBytes(a[1]); var now = ms.ToArray();
                int diff = -1; for (int i = 0; i < Math.Min(orig.Length, now.Length); i++) if (orig[i] != now[i]) { diff = i; break; }
                Console.WriteLine($"orig={orig.Length} rebuilt={now.Length} firstDiff={(diff < 0 && orig.Length == now.Length ? "none (identical)" : diff.ToString("x"))}");
                File.Delete(tmp); File.Delete(tmp + ".RDB2");
                return diff < 0 && orig.Length == now.Length ? 0 : 1;
            }
        }
        return 2;
    }
}
