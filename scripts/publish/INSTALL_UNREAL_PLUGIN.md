# Install Smatchet Unreal Plugin

Project install:

```powershell
.\scripts\publish\install_unreal_plugin.ps1 -ProjectRoot "C:\Path\To\YourProject"
```

Engine install:

```powershell
.\scripts\publish\install_unreal_plugin.ps1 -EngineRoot "C:\Program Files\Epic Games\UE_5.5"
```

If the destination already has an older copy of the plugin, re-run with `-Force`.
