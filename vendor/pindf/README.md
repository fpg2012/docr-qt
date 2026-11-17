# pindf (Rust)

一个用 Rust 重写的 PDF 解析与增量修改库，对应 C 版
[`libpindf`](../pindf)。保留了 C 版的整体流程（词法分析 → 对象解析 →
xref/文档 → 懒加载），但用 Rust 的所有权模型消除了原库中大量的手工
`malloc/free`、裸指针共享和生命周期约定。

C 版的结构分析与所有权问题清单见
[`docs/c-design-analysis.md`](docs/c-design-analysis.md)，
迁移进度见 [`docs/migration-checklist.md`](docs/migration-checklist.md)。

## 设计要点

| C 版 | Rust 版 |
| --- | --- |
| `pindf_uchar_str` + 手写 `len/capacity` | `Vec<u8>` |
| 按 `elem_size` 拷贝裸字节的 `pindf_vector` | `Vec<Object>`、`BTreeMap`、`HashMap` |
| `pindf_pdf_obj` tag-union + 递归 `destroy` | `enum Object`，`Clone` 即深拷贝，`Drop` 自动释放 |
| 字典 = keys/values 两个平行 vector | `Dictionary(BTreeMap<Vec<u8>, Object>)`，天然去重/有序 |
| 移进-归约 PDA + 符号栈 | 递归下降解析器 |
| `token`/`raw_str` 的所有权靠注释 | token 借用输入切片，字符串在词法层解码成 `Vec<u8>` |
| `FILE*` + `fseek` + 外部 lexer/parser | `Document::open` 按窗口 `seek+read`，对象仍懒加载；`parse` 用内存缓冲 |
| 负错误码 + out-parameter | `Result<T, Error>`（带字节偏移） |
| 定长 1 MiB 流缓冲、定长序列化缓冲 | 全部动态增长 |
| 交叉引用流/对象流部分支持 | 传统 xref 表、xref 流、对象流、混合引用文件 |

`Document::open(path)` 只保存文件句柄和长度；解析对象时按 xref 偏移 `seek` 读取一个窗口，
窗口不够就倍增重试，不会在打开时把整个文件读入内存。`Document::parse(Vec<u8>)` 用于调用方
已经持有完整字节的场景。

除此之外还修正了若干 C 版 bug：

- 字典/数组的 trailer 双重所有权；
- 用未初始化的 `uchar_str` 缓冲做 `atoi/atof`（实测会把 `/Rotate 0` 读成 2）；
- xref 项“先写者赢”依赖全零哨兵；
- `atoi`/`%s` 对非 NUL 结尾数据的使用；
- 对象号 0 与“未加载”哨兵冲突；
- `/Size` 的 `+2` 约定；
- 固定缓冲导致的截断和越界。

## 构建与测试

```sh
cargo build --release
cargo test
```

依赖只有 [`flate2`](https://crates.io/crates/flate2)（FlateDecode）。

## 作为库使用

```rust
use pindf::{Dictionary, Document, Object};

fn main() -> pindf::Result<()> {
    let mut doc = Document::open("input.pdf")?;
    println!("version = {}", doc.version());

    // 读取 trailer 里的 /Root 并解引用
    let root = doc.trailer().get("Root").and_then(Object::as_ref).unwrap();
    let catalog = doc.resolve(root)?.as_dict().unwrap();
    println!("pages = {:?}", catalog.get("Pages"));

    // 追加一个对象并保存（原文件 + 增量）
    let mut dict = Dictionary::new();
    dict.insert("Type", Object::name("Example"));
    dict.insert("Value", Object::Int(42));
    doc.modification_mut().add(100, Object::Dict(dict));
    doc.save("output.pdf")?;
    Ok(())
}
```

借用说明：`Document::object` / `resolve` / `catalog` 返回 `&Object`，借用期间
不能再次可变借用文档。需要同时持有多个对象时先 `clone()`（等价于 C 的
`pindf_pdf_obj_deepcopy`），或只保存 `Ref` 再逐个解析。C 版
`pindf_deref(doc, obj)` 直接返回借用指针，在 Rust 中无法安全表达，因此改为
`doc.resolve(reference)`。

### 主要 API

```rust
// 解析
Document::open(path) / Document::parse(Vec<u8>)
doc.version() / doc.trailer() / doc.xref() / doc.source_len() / doc.read_all()
doc.object(num) -> &Object
doc.indirect_object(num) -> &IndirectObject
doc.resolve(Ref) -> &Object
doc.catalog() -> &Object
doc.is_encrypted() -> bool

// 对象
Object::{Null, Bool, Int, Real, Name, String, HexString, Array, Dict, Stream, Reference}
Dictionary::{get, insert, remove, get_int, get_name, get_dict, get_array}
Stream::decode() -> Vec<u8>       // 应用 /Filter、/DecodeParms
serialize::object_to_bytes(&obj)  // 写回 PDF 语法
serialize::to_json(&obj)

// 增量修改
doc.modification_mut().add(num, object)
doc.write_incremental(&mut writer, base_offset, compress_xref) -> u64
doc.save(path)                    // 复制原文件并追加增量
```

## 命令行工具

```sh
# 打印 token 流（对应 C 的 lexer_test）
cargo run --bin pindf-lex -- input.pdf

# 把所有间接对象和 trailer 导出成 JSON（对应 C 的 parser_test）
cargo run --bin pindf-json -- input.pdf out.json

# 全量校验：加载所有在用对象；--decode 会解码所有流
cargo run --bin pindf-check -- input.pdf --decode

# 只读取一个对象（文件后端按需 seek，不加载其他对象）
cargo run --bin pindf-object -- input.pdf 42 --decode

# 追加一个书签并写出新文件（对应 C 的 doc_save_modif_test）
cargo run --bin pindf-outline -- input.pdf output.pdf
```

## 已实现 / 未实现

已实现：

- 传统 xref 表、xref 流（`/W`、`/Index`、`/Prev`）、混合引用文件 `/XRefStm`
- 对象流 `/ObjStm`（懒加载、按 header 偏移解析）
- 所有基本对象类型、`n g R` 引用、直接/间接 `/Length`
- 过滤器：FlateDecode、ASCIIHexDecode、ASCII85Decode、RunLengthDecode、LZWDecode
- PNG predictor（None/Sub/Up/Average/Paeth）与 TIFF predictor 2
- 增量保存：传统 xref 表与压缩 xref 流
- PDF → 字节流 / JSON 序列化

未实现（会返回 `Error::Unsupported` 或明确报错）：

- 加密（`/Encrypt`，RC4/AES）；`is_encrypted()` 只做标记
- DCTDecode / JPXDecode / CCITTFaxDecode
- 对象流 `/Extends`
- xref 流中间接引用的 `/Filter` / `/DecodeParms`
- 损坏 xref 的全文件扫描重建、linearized PDF 写回

> 注意：上面的 DCT/JPX/CCITT 是**流的编码器**，不是“不支持流”。图像在 PDF 里就是
> 普通 `Stream`：对象和字典会完整解析，原始编码字节在 `stream.data` 中。`DCTDecode`
> 数据就是 JPEG 字节，可以直接交给图像库；`FlateDecode` 的图像数据（包括 PNG
> predictor）本库可以正常解码成原始像素样本。

## 目录

```
src/
  error.rs        错误类型
  object.rs       对象模型（Object/Dictionary/Stream/Ref/IndirectObject）
  lexer.rs        字节切片词法分析
  parser.rs       递归下降解析
  xref.rs         xref 表
  source.rs       内存 / 文件窗口读取
  document.rs     文档、xref 链、懒加载、增量保存
  stream.rs       过滤器与 predictor
  serialize.rs    PDF / JSON 序列化
  modification.rs 增量修改集
  bin/            示例工具
tests/            词法、解析、文档、对象流、保存等集成测试
docs/             C 版分析与迁移清单
```

## 真实文件验证

在本地真实 PDF 上跑过 `pindf-check`：

- arXiv 论文（275 个对象、62 个 Flate 流）：273 个在用对象全部加载，0 错误；
  62 个流全部解码，共约 44 MB，耗时约 30 ms。
- 58 页线性化 PDF（2041 个对象、553 个流）：2040 个对象全部加载；548 个流解码，
  5 个 `/DCTDecode` JPEG 按设计返回 `Unsupported`。
- 3 页中文 PDF、10 页论文、扫描件：结构全部加载；扫描件的 4 个
  `/CCITTFaxDecode` 按设计返回 `Unsupported`。
- 用 `pindf-outline` 给 arXiv 论文追加书签后，`qpdf --check` 无错误，
  qpdf 能列出新增的 “Random Outline”。
- 598 MiB 扫描书（626510068 字节、476 个对象）：只读取 Catalog 对象时
  `pindf-object` 峰值 RSS 约 15.8 MiB、耗时约 1.2 ms，证明打开文件不会整体加载。

## License

Apache-2.0，与 C 版一致。见 [`LICENSE`](LICENSE)。
