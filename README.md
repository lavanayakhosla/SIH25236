
```markdown
# 🔒 LLVM-Based Object File Obfuscator

> **A cross-platform software obfuscation tool** built using LLVM that takes C/C++ source code, compiles it into LLVM IR, applies custom obfuscation passes, and generates highly obfuscated binaries for Windows and Linux.

---

## 🧠 Background

Software obfuscation has become a crucial technique in modern software engineering — especially where **intellectual property protection**, **reverse engineering prevention**, and **software piracy mitigation** are essential.

This project leverages **LLVM (Low-Level Virtual Machine)** — a modular compiler infrastructure — to generate obfuscated object files and binaries from C/C++ code.

---

## 🧩 Key Features

✅ Obfuscates binaries for **Windows and Linux**  
✅ Adjustable **levels of obfuscation** via command-line parameters  
✅ Generates **detailed obfuscation report**  
✅ Supports:
- Control flow flattening
- Bogus code insertion
- String encryption
- Fake loop insertion
- NOP padding

✅ Produces:
- **Obfuscated binary**
- **Comprehensive log report**

---

## 📁 Project Structure

```

llvm-obfuscator/
├── llvm_pass/              # LLVM obfuscation pass source code
│   ├── CMakeLists.txt
│   └── ObfuscationPass.cpp
├── driver/                 # Python driver that orchestrates compilation & obfuscation
│   ├── obfuscator.py
│   ├── requirements.txt
│   └── obfpass.dll/.so
├── examples/               # Sample input programs
│   └── hello.c
├── build/                  # Build directory for LLVM pass
└── README.md

````

---

## ⚙️ Installation & Setup

### 🧱 Prerequisites

Make sure you have:
- **LLVM (>= 14)** with `clang`, `opt`, `llc`, `llvm-link`
- **CMake** (>= 3.10)
- **Python 3**
- **g++** or **clang++** (for linking)
- **make**

---

### 🧑‍💻 Setup on Linux / macOS

```bash
# 1. Install dependencies
sudo apt update
sudo apt install -y clang llvm cmake make python3 python3-pip

# 2. Clone repository
git clone https://github.com/<your-username>/llvm-obfuscator.git
cd llvm-obfuscator

# 3. Build LLVM pass
mkdir build && cd build
cmake -DLLVM_DIR=/usr/lib/llvm-14/lib/cmake/llvm ..
make

# 4. Run obfuscator
cd ../driver
python3 -m pip install -r requirements.txt
python3 obfuscator.py ../examples/hello.c --out hello_obf --pass ../build/llvm_pass/obfpass.so --bogus-blocks 2 --string-level 2 --insert-nops 5

# 5. Run output binary
./hello_obf
````

---

### 🪟 Setup on Windows

**Requirements:**

* Visual Studio 2022 (with C++ workload)
* LLVM for Windows
* CMake
* Python 3

**Commands:**

```powershell
# In Developer PowerShell for VS 2022
$env:LLVM_DIR="C:\Program Files\LLVM\lib\cmake\llvm"
$env:Path="C:\Program Files\LLVM\bin;" + $env:Path

cd llvm-obfuscator
mkdir build; cd build
cmake -G "Visual Studio 17 2022" -A x64 -DLLVM_DIR="$env:LLVM_DIR" ..
cmake --build . --config Release

Copy-Item -Path ".\llvm_pass\Release\obfpass.dll" -Destination "..\driver\obfpass.dll" -Force
cd ..\driver

python -m pip install -r requirements.txt
python obfuscator.py ..\examples\hello.c --out hello_obf.exe --pass .\obfpass.dll --bogus-blocks 2 --string-level 2 --insert-nops 5 --target windows
.\hello_obf.exe
```

---

## 🧾 Output

### 1. **Obfuscated Binary**

A fully obfuscated executable is generated — resistant to reverse engineering and decompilation.

### 2. **Report File**

A detailed report `obfuscation_report.txt` is generated containing:

| Parameter          | Description                            |
| ------------------ | -------------------------------------- |
| Input Parameters   | User-selected options for obfuscation  |
| Output Attributes  | File size, obfuscation methods applied |
| Bogus Code         | Amount of fake code inserted           |
| Cycles Completed   | Number of obfuscation iterations       |
| String Obfuscation | Number of encrypted strings            |
| Fake Loops         | Number of dummy loops added            |

---

## 🧪 Example

### Input:

```c
#include <stdio.h>
int main() {
    printf("Hello, world!\n");
    return 0;
}
```

### Run:

```bash
python3 obfuscator.py ../examples/hello.c --out hello_obf --bogus-blocks 3 --string-level 2
```

### Output:

```
[INFO] Obfuscation started...
[INFO] Generating LLVM IR...
[INFO] Applying custom obfuscation pass...
[INFO] Linking and emitting final binary...
[INFO] Obfuscation completed successfully.

Output file: hello_obf
Report file: obfuscation_report.txt
```

---

## 📊 Example Report

```
===== OBFUSCATION REPORT =====
Input file: hello.c
Output file: hello_obf
Platform: Linux
Obfuscation Cycles: 2
Bogus Blocks Inserted: 20
Fake Loops: 5
Strings Encrypted: 3
NOP Instructions Added: 50
Output File Size: 37 KB
==============================
```

---

## 🧰 Command-Line Options

| Option                     | Description                              |
| -------------------------- | ---------------------------------------- |
| `--out <filename>`         | Name of the output binary                |
| `--pass <path>`            | Path to LLVM obfuscation pass (.so/.dll) |
| `--bogus-blocks <n>`       | Number of bogus code blocks to insert    |
| `--string-level <n>`       | Level of string obfuscation/encryption   |
| `--insert-nops <n>`        | Number of NOP instructions to insert     |
| `--target <windows/linux>` | Output binary platform                   |
| `--cycles <n>`             | Number of obfuscation iterations         |

---

## 🧤 Troubleshooting

### 🧩 “cannot load library obfpass.so”

* Ensure the plugin path is correct
* Make sure the plugin is built with the same LLVM version
* Use absolute paths to `.so` / `.dll` files

### ⚙️ “opt: command not found”

* Add LLVM binaries to your PATH:

  ```bash
  export PATH=/usr/lib/llvm-14/bin:$PATH
  ```

### 🚫 “Architecture mismatch”

* Build everything with the same architecture (`x64` for both LLVM and the plugin).

---

## 🏗️ Future Enhancements

* Add GUI for parameter configuration
* Add dynamic control-flow obfuscation
* Add support for multiple obfuscation passes in one run
* Integration with CI/CD for secure builds

---



