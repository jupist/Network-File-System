# File Distribution Across Storage Servers

## ❓ The Question

**"When I create a file, which storage server does it go to? How does the system decide?"**

---

## 🔍 Previous Implementation (BEFORE)

### The Problem
```c
// Old code - ALWAYS used server_list[0]
int ss_nm_port = server_list[0].ss_nm_port;
```

**Result:**
- ❌ ALL files went to Storage Server 0 (SS0)
- ❌ SS1, SS2, SS3, etc. never stored primary files
- ❌ No load distribution!
- ❌ Poor utilization of multiple storage servers

**Example:**
```
CREATE file1.txt  →  SS0
CREATE file2.txt  →  SS0  (should be SS1!)
CREATE file3.txt  →  SS0  (should be SS2!)
CREATE file4.txt  →  SS0  (should be SS0 again)
```

---

## ✅ NEW Implementation (AFTER - Round-Robin)

### The Solution
```c
// Global counter for round-robin distribution
int g_next_ss_for_file = 0;

// In CREATE handler:
int selected_ss = g_next_ss_for_file;
g_next_ss_for_file = (g_next_ss_for_file + 1) % g_num_servers;

// Use the selected SS
int ss_nm_port = server_list[selected_ss].ss_nm_port;
```

**Result:**
- ✅ Files distributed evenly across ALL storage servers
- ✅ Fair load balancing
- ✅ Better resource utilization
- ✅ Simple and predictable

**Example with 3 Storage Servers:**
```
CREATE file1.txt  →  SS0  (counter: 0 → 1)
CREATE file2.txt  →  SS1  (counter: 1 → 2)
CREATE file3.txt  →  SS2  (counter: 2 → 0)
CREATE file4.txt  →  SS0  (counter: 0 → 1)  ← Back to start!
CREATE file5.txt  →  SS1  (counter: 1 → 2)
CREATE file6.txt  →  SS2  (counter: 2 → 0)
```

---

## 📊 How Round-Robin Works

### Visual Example with 4 Storage Servers:

```
Counter starts at 0
│
├─ CREATE file1.txt
│  └─> Select SS[0]  →  SS0
│      Counter = (0 + 1) % 4 = 1
│
├─ CREATE file2.txt
│  └─> Select SS[1]  →  SS1
│      Counter = (1 + 1) % 4 = 2
│
├─ CREATE file3.txt
│  └─> Select SS[2]  →  SS2
│      Counter = (2 + 1) % 4 = 3
│
├─ CREATE file4.txt
│  └─> Select SS[3]  →  SS3
│      Counter = (3 + 1) % 4 = 0  ← Wraps around!
│
└─ CREATE file5.txt
   └─> Select SS[0]  →  SS0  ← Back to beginning
       Counter = (0 + 1) % 4 = 1
```

---

## 🎯 Distribution Patterns

### Scenario 1: 2 Storage Servers (SS0, SS1)

| File Created | Goes To | Counter |
|--------------|---------|---------|
| file1.txt    | SS0     | 0 → 1   |
| file2.txt    | SS1     | 1 → 0   |
| file3.txt    | SS0     | 0 → 1   |
| file4.txt    | SS1     | 1 → 0   |
| file5.txt    | SS0     | 0 → 1   |

**Result:** Perfect 50/50 split

---

### Scenario 2: 3 Storage Servers (SS0, SS1, SS2)

| File Created | Goes To | Counter |
|--------------|---------|---------|
| file1.txt    | SS0     | 0 → 1   |
| file2.txt    | SS1     | 1 → 2   |
| file3.txt    | SS2     | 2 → 0   |
| file4.txt    | SS0     | 0 → 1   |
| file5.txt    | SS1     | 1 → 2   |
| file6.txt    | SS2     | 2 → 0   |

**Result:** Perfect 33/33/33 split

---

### Scenario 3: 5 Storage Servers (SS0, SS1, SS2, SS3, SS4)

| File Created | Goes To | Counter |
|--------------|---------|---------|
| file1.txt    | SS0     | 0 → 1   |
| file2.txt    | SS1     | 1 → 2   |
| file3.txt    | SS2     | 2 → 3   |
| file4.txt    | SS3     | 3 → 4   |
| file5.txt    | SS4     | 4 → 0   |
| file6.txt    | SS0     | 0 → 1   |

**Result:** Perfect 20% per server

---

## 🔄 Combined with Replication

### Example with 4 Storage Servers:

```
Storage Servers:
  SS0 ← replicated by SS1
  SS2 ← replicated by SS3

File Creation Sequence:
```

| File | Primary Location | Replica Location | Round-Robin Counter |
|------|------------------|------------------|---------------------|
| file1.txt | SS0 | SS1 | 0 → 1 |
| file2.txt | SS1 | SS0 | 1 → 2 |
| file3.txt | SS2 | SS3 | 2 → 3 |
| file4.txt | SS3 | SS2 | 3 → 0 |
| file5.txt | SS0 | SS1 | 0 → 1 |

**Result:**
- Even distribution of primary files
- Each SS stores some primary files AND some replicas
- Good fault tolerance AND load balancing

---

## 📂 What You'll See in Storage Directories

After creating 6 files with 3 Storage Servers:

### ss0_storage/
```
file1.txt  ← Primary
file4.txt  ← Primary
file2.txt  ← Replica (from SS1)
file5.txt  ← Replica (from SS1)
```

### ss1_storage/
```
file2.txt  ← Primary
file5.txt  ← Primary
file1.txt  ← Replica (from SS0)
file4.txt  ← Replica (from SS0)
```

### ss2_storage/
```
file3.txt  ← Primary
file6.txt  ← Primary
```

---

## 🎯 Advantages of Round-Robin

### ✅ Pros:
1. **Simple** - Easy to implement and understand
2. **Fair** - Perfect even distribution
3. **Predictable** - You know exactly where files will go
4. **No hotspots** - All servers get equal load
5. **Fast** - O(1) decision time

### ❌ Cons:
1. **No consideration of current load** - Doesn't check which SS is busier
2. **No file size awareness** - Doesn't consider some files might be huge
3. **Sequential** - Might create patterns if files are created in bursts

---

## 🔧 Alternative Strategies (Not Implemented)

### 1. Hash-Based
```c
int selected_ss = hash(filename) % g_num_servers;
```
- Same file always goes to same SS (deterministic)
- Good for consistency
- May create uneven distribution

### 2. Load-Based
```c
// Choose SS with fewest files
int selected_ss = find_ss_with_min_files();
```
- Dynamic balancing
- Considers actual load
- More complex, slower

### 3. Random
```c
int selected_ss = rand() % g_num_servers;
```
- Unpredictable
- Statistically even over time
- Not deterministic

---

## 📝 Testing the Distribution

### Test Script:
```bash
./start_servers.sh  # Start NS + SS0 + SS1

./client
> alice
> CREATE file1.txt  # Should go to SS0
> CREATE file2.txt  # Should go to SS1
> CREATE file3.txt  # Should go to SS0
> CREATE file4.txt  # Should go to SS1
> EXIT

# Check distribution:
ls -la ss0_storage/  # Should have file1.txt, file3.txt
ls -la ss1_storage/  # Should have file2.txt, file4.txt
```

### Name Server Log Output:
```
Assigning 'file1.txt' to SS0 (Round-robin)
Assigning 'file2.txt' to SS1 (Round-robin)
Assigning 'file3.txt' to SS0 (Round-robin)
Assigning 'file4.txt' to SS1 (Round-robin)
```

---

## 🎓 Summary

**Question:** "Which storage server does a file go to?"

**Answer:** Files are distributed in **round-robin fashion** across all available storage servers:
- File 1 → SS0
- File 2 → SS1
- File 3 → SS2
- File 4 → SS0 (wraps around)
- etc.

This ensures:
- ✅ Even distribution
- ✅ All storage servers are utilized
- ✅ Balanced load
- ✅ Simple and predictable behavior

**Plus:** Each file is also **replicated asynchronously** to its designated replica server for fault tolerance!
