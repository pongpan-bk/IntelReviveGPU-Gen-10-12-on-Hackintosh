# ===========================================================================
#  setup_vmware.ps1
#  VMware Workstation Pro — macOS VM Auto-Configuration
#
#  รันบน Windows ด้วย PowerShell (Administrator)
#  สร้าง VM สำหรับ macOS Sonoma Sequoia เพื่อ compile + load MyIntelGPU.kext
#
#  Requirements:
#    1. VMware Workstation Pro 17+ (หรือ Player พร้อม unlocker)
#    2. macOS installer (.iso หรือ .dmg) — หาจาก Internet Recovery
#    3. Python (สำหรับ unlocker)
#
#  Usage:
#    .\setup_vmware.ps1
#
#  Reference:
#    https://github.com/DrDonk/unlocker
#    https://dortania.github.io/OpenCore-Install-Guide/
# ===========================================================================

param(
    [string]$VmName        = "macOS-Sonoma",
    [string]$VmDir         = "$env:USERPROFILE\Documents\Virtual Machines\$VmName",
    [string]$MacOSIso      = "",
    [string]$DiskSizeGB    = 80,
    [int]    $RamMB        = 8192,
    [int]    $Vcpus        = 4
)

$VmxPath    = "$VmDir\$VmName.vmx"
$UnlockerDir = "$env:USERPROFILE\Desktop\unlocker"

Write-Host "=== MyIntelGPU — VMware macOS VM Setup ===" -ForegroundColor Cyan

# ─────────────────────────────────────────────────────────────
# Step 1: Check prerequisites
# ─────────────────────────────────────────────────────────────
Write-Host "`n[1/5] Checking prerequisites..." -ForegroundColor Yellow

$vmwarePath = Get-Command "vmware.exe" -ErrorAction SilentlyContinue
if (-not $vmwarePath) {
    Write-Warning "VMware Workstation not found in PATH."
    Write-Host "  Download: https://www.vmware.com/products/workstation-pro/"
    Write-Host "  Or use (OOB) => OOB VMware Workstation Player + unlocker"
}

# ─────────────────────────────────────────────────────────────
# Step 2: Download & run unlocker
# ─────────────────────────────────────────────────────────────
Write-Host "`n[2/5] Installing VMware Unlocker (macOS guest support)..." -ForegroundColor Yellow

if (-not (Test-Path "$UnlockerDir\win-install.cmd")) {
    Write-Host "  Downloading unlocker from github.com/DrDonk/unlocker..."

    # ใช้ portable 7-zip ถ้ามี หรือ zip
    $unlockerUrl = "https://github.com/DrDonk/unlocker/archive/refs/heads/master.zip"
    $zipFile = "$env:TEMP\unlocker.zip"

    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $unlockerUrl -OutFile $zipFile

    Expand-Archive -Path $zipFile -DestinationPath "$env:USERPROFILE\Desktop" -Force
    Rename-Item "$env:USERPROFILE\Desktop\unlocker-master" $UnlockerDir -Force

    Write-Host "  Running unlocker (ต้องปิด VMware ก่อน)..." -ForegroundColor Red
    Write-Host "  กด Enter เพื่อรัน unlocker หรือ Ctrl+C เพื่อรันเองทีหลัง"
    Read-Host
    Push-Location $UnlockerDir
    cmd /c "win-install.cmd"
    Pop-Location
} else {
    Write-Host "  Unlocker already installed."
}

# ─────────────────────────────────────────────────────────────
# Step 3: Create VM directory
# ─────────────────────────────────────────────────────────────
Write-Host "`n[3/5] Creating VM directory..." -ForegroundColor Yellow
New-Item -ItemType Directory -Path $VmDir -Force | Out-Null

# ─────────────────────────────────────────────────────────────
# Step 4: Generate .vmx configuration
# ─────────────────────────────────────────────────────────────
Write-Host "`n[4/5] Generating .vmx configuration..." -ForegroundColor Yellow

# Board ID ปลอม — ใช้ของ iMac19,1 (Coffee Lake) เพื่อให้ kext ของเรา
# เหมือนทำงานบน Coffee Lake จริง ๆ (FakeID ทำงาน)
$boardId = "Mac-AA95B1DDAB278B95"  # iMac19,1 — Coffee Lake

$vmxContent = @"
.encoding = "windows-1252"
config.version = "8"
virtualHW.version = "21"
vmcio.enable = "TRUE"

# ─── Guest OS ───
guestOS = "darwin22-64"
board-id = "$boardId"
hw.model = "iMac19,1"
serialNumber = "C02XK0A8JG5H"

# ─── CPU ───
numvcpus = "$Vcpus"
cpuid.coresPerSocket = "1"
cpuid.0.eax = "0000:0000:0000:0000:0000:0000:0000:1011"
cpuid.0.ebx = "0111:0101:0110:1110:0110:0101:0100:0111"
cpuid.0.ecx = "0110:1100:0110:0101:0111:0100:0110:1110"
cpuid.0.edx = "0100:1001:0110:0101:0111:0100:0110:1110"
cpuid.1.eax = "0000:0000:0000:0001:0000:0110:0111:0001"
cpuid.1.ebx = "0000:0010:0000:0001:0000:1000:0000:0000"
cpuid.1.ecx = "1000:0010:1001:1000:0010:0010:0000:0011"
cpuid.1.edx = "0000:0111:1000:1011:1111:1011:1111:1111"

# ─── Memory ───
memsize = "$RamMB"

# ─── Storage ───
scsi0.present = "TRUE"
scsi0.virtualDev = "pvscsi"
scsi0:0.present = "TRUE"
scsi0:0.fileName = "$VmName.vmdk"
scsi0:0.mode = "persistent"

"@

# เพิ่ม DVD/CD drive ถ้าระบุ ISO
if ($MacOSIso -and (Test-Path $MacOSIso)) {
    $vmxContent += @"
ide1:0.present = "TRUE"
ide1:0.fileName = "$MacOSIso"
ide1:0.deviceType = "cdrom-image"
ide1:0.autodetect = "TRUE"

"@
} else {
    $vmxContent += @"
ide1:0.present = "TRUE"
ide1:0.deviceType = "cdrom-raw"
ide1:0.autodetect = "TRUE"
ide1:0.startConnected = "TRUE"

"@
}

$vmxContent += @"
# ─── Network ───
ethernet0.present = "TRUE"
ethernet0.virtualDev = "vmxnet3"
ethernet0.connectionType = "nat"
ethernet0.startConnected = "TRUE"

# ─── Graphics (สำหรับ macOS UI) ───
svga.present = "TRUE"
svga.autodetect = "TRUE"
svga.guestBackedPrimary = "TRUE"
mks.enable3d = "TRUE"
mks.vk_allow_cpu = "TRUE"

# ─── USB + Misc ───
usb.present = "TRUE"
usb.generic.autoconnect = "FALSE"
sound.present = "TRUE"
sound.virtualDev = "hdaudio"
keyboard.present = "TRUE"
mouse.present = "TRUE"

# ─── Power Management (ป้องกัน sleep ขณะ build) ───
powerType.powerOff = "soft"
powerType.reset = "soft"
powerType.suspend = "soft"

# ─── smc (Apple SMC — ต้องใช้ unlocker ถึงจะใช้ได้) ───
smc.present = "TRUE"
smc.version = "0"

# ─── Advanced ───
isolation.tools.hgfs.disable = "FALSE"
isolation.tools.dnd.disable = "TRUE"
isolation.tools.copy.disable = "TRUE"
isolation.tools.paste.disable = "TRUE"
vmx.log.file = "$VmName.log"
log.fileName = "$VmName.log"

# ─── MyIntelGPU Kext Development share ───
# แชร์โฟลเดอร์ kext project เข้า VM
sharedFolder0.present = "TRUE"
sharedFolder0.enabled = "TRUE"
sharedFolder0.readAccess = "TRUE"
sharedFolder0.writeAccess = "TRUE"
sharedFolder0.hostPath = "$(Get-Location)"
sharedFolder0.guestName = "MyIntelGPU"
sharedFolder0.expiration = "never"
hgfs.mapRootShare = "TRUE"
hgfs.linkRootShare = "TRUE"

# ─── Serial port for kernel debug ───
serial0.present = "TRUE"
serial0.fileType = "pipe"
serial0.fileName = "\\.\pipe\$(VmName)_debug"
serial0.yieldOnMsrRead = "TRUE"
serial0.startConnected = "TRUE"
"@

Set-Content -Path $VmxPath -Value $vmxContent -Encoding ASCII
Write-Host "  VMX written: $VmxPath" -ForegroundColor Green

# ─────────────────────────────────────────────────────────────
# Step 5: Create empty VMDK + instructions
# ─────────────────────────────────────────────────────────────
Write-Host "`n[5/5] Creating virtual disk..." -ForegroundColor Yellow

$vdiskman = "${env:ProgramFiles}\VMware\VMware Workstation\vmware-vdiskmanager.exe"
if (Test-Path $vdiskman) {
    & $vdiskman -c -s "${DiskSizeGB}GB" -a lsilogic -t 0 "$VmDir\$VmName.vmdk" 2>&1 | Out-Null
    Write-Host "  Virtual disk created: ${DiskSizeGB}GB" -ForegroundColor Green
} else {
    Write-Host "  [!] vmware-vdiskmanager not found." -ForegroundColor Yellow
    Write-Host "  Create disk manually:" -ForegroundColor Gray
    Write-Host "    >  File → New Virtual Machine → Use existing .vmx" -ForegroundColor Gray
}

# ─────────────────────────────────────────────────────────────
# Done
# ─────────────────────────────────────────────────────────────
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "✅ VMware VM created!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "VMX     : $VmxPath"
Write-Host "RAM     : ${RamMB}MB"
Write-Host "CPU     : ${Vcpus} vCPU"
Write-Host "Disk    : ${DiskSizeGB}GB"
Write-Host "Board ID: $boardId (Coffee Lake iMac19,1)"
Write-Host "Share   : host source → VM /mnt/hgfs/MyIntelGPU/"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Open .vmx ใน VMware Workstation"
Write-Host "  2. Boot จาก macOS installer (Internet Recovery หรือ ISO)"
Write-Host "  3. Install macOS"
Write-Host "  4. ภายใน VM: cd /mnt/hgfs/MyIntelGPU && make"
Write-Host "  5. sudo kextutil MyIntelGPU.kext"
Write-Host ""
Write-Host "Tips:"
Write-Host "  - ถ้า boot ไม่ขึ้น → แก้ board-id ใน .vmx"
Write-Host "  - ถ้าต้องการ macOS ISO:"
Write-Host "    > git clone https://github.com/sickcodes/osx-iso"
Write-Host "    > cd osx-iso && ./fetch-iso.sh"
Write-Host "========================================" -ForegroundColor Cyan
