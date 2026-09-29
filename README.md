# WLM - Wine Launch Manager

Wine Launch Manager (WLM) is a GTK3-based application for managing Vanilla Wine applications on Linux distributions.

---

## Screenshot
![Screenshot WLM](WLM_SS/2.png)
---

## How to Use WLM?

### **Before Using, Ensure:**

1. You have installed Wine Vanilla correctly according to your distro.
2. You have installed the following packages:
   ```libgtk-3-0 libcurl4 libarchive13 libwebkit2gtk-4.1-0```
   *(Use the commands below or adjust according to your distribution.)*

---

## Install the required components or packages

### **Debian / Ubuntu / Linux Mint**
```bash
sudo apt install ibgtk-3-0 libcurl4 libarchive13 libwebkit2gtk-4.1-0
```

### **Arch Linux / Manjaro**
```bash
sudo pacman -Syu
sudo pacman -S gtk3 curl libarchive webkit2gtk-4.1
```

### **Fedora**
```bash
sudo dnf install gtk3 libcurl libarchive webkit2gtk4.1
```

---

## Steps to Run WLM:

1. Download the latest version of WLM.
2. Open a terminal in the directory where the file has been downloaded (e.g., `~/Downloads`).
3. Extract the archive and move it to your home directory:
   ```bash
   tar -xf WLM_version.tar.gz -C ~/
   ```
4. Navigate to the WLM directory:
   ```bash
   cd ~/wlm/
   ```
5. Run the WLM script:
   ```bash
   ./WLM.sh
   ```

**Note:** Alternatively, you can extract the archive using your file manager, navigate to the Home directory (`~/`), and double-click `WLM.sh` to run it.

---

## Features:

- Manage Vanilla Wine applications via a user-friendly GUI.
- Uninstall applications installed within Wine.
- Display FPS using GalliumHUD or MangoHUD.
- Create and manage shortcut lists in the Launcher.
- GOG Integration.
- Manage Prefixes for Wine and Proton.
- Download ProtonGE/ProtonCachyOS within the launcher.
  
---
## How to Play?
1. **Play Button**: Runs the application that has been added to the shortcut list.
2. **Rename Button**: Renames the shortcut in the list.
3. **Remove Button**: Deletes an application from the shortcut list.
4. **Add Button**: Adds an application to the shortcut list menu (.exe file).
5. **Change Icon Button**: Changes the launcher icon (*.ico, *.png).
6. **Launch Mode Button**: For Counter FPS using GalliumHUD & Mangohud (GL or VK)

## WLM Settings
![Screenshot WLM](WLM_SS/5.png)
### **Settings Menu:**

- **Winecfg Button**: Opens the Wine Vanilla configuration.
- **Open Wine Prefix Folder**: Opens Wine Prefix Folder.
- **Uninstaller**: Uninstalls programs installed within Wine.
- **Wine Explorer**: Opens the file manager or explorer inside Wine.
- **Refresh**: Just Refresh.

---

## How to Uninstall WLM?

### **Safer Method (File Manager):**

Simply delete the `wlm` directory using your file manager:

```
~/wlm
```

### **Terminal Method:**

```bash
rm -rf ~/wlm
```

---
