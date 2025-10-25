Prime Threads — Quick Build & Run (config.ini-only)

Visual Studio (Windows) — Debug (recommended)
1) Open the solution. Set Configuration=Debug, Platform=x64.
2) Add your single file: config.ini
   • Properties → Item Type=Text
   • Content=Yes
   • Copy to Output Directory=Copy if newer  (set for “All Configurations” if you later use Release)
3) Press “Local Windows Debugger” (F5).

How it runs
• On launch, pick a variant in the menu:
   1 = A1B1 (Immediate + Range)
   2 = A2B1 (Deferred  + Range)
   3 = A1B2 (Immediate + Per-number)
   4 = A2B2 (Deferred  + Per-number)
• Only config.ini is needed; variants are chosen at runtime.
• If config.ini is missing, the program uses defaults.

config.ini (example)
threads=12        # set to your logical processors
max_value=65536   # search upper bound

Optional (Release)
• Switch to Configuration=Release, x64.
• Ensure config.ini still has “Copy to Output Directory=Copy if newer”.
• Clean → Rebuild; run with Ctrl+F5.
