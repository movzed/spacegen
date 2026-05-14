const { app, BrowserWindow, ipcMain, dialog } = require('electron')
const path = require('path')
const fs = require('fs')

let mainWindow

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1920,
    height: 1080,
    backgroundColor: '#000000',
    webPreferences: {
      nodeIntegration: true,
      contextIsolation: false,
      webSecurity: false,
    },
  })
  mainWindow.loadFile(path.join(__dirname, '../renderer/index.html'))
  if (process.argv.includes('--dev')) {
    mainWindow.webContents.openDevTools({ mode: 'detach' })
  }
}

app.whenReady().then(createWindow)
app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit() })
app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow() })

ipcMain.handle('dialog:openDirectory', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openDirectory'],
    title: 'Select Mask PNG Sequence Folder',
  })
  return result.canceled ? null : result.filePaths[0]
})

ipcMain.handle('fs:readDir', async (_e, dirPath) => {
  try {
    return fs.readdirSync(dirPath)
      .filter(f => /\.(png|jpg|jpeg)$/i.test(f))
      .sort()
      .map(f => path.join(dirPath, f))
  } catch {
    return []
  }
})
