vim.lsp.config('clangd', {
  cmd = {
    'clangd',
    '-j=16',
    '--background-index',
    '--clang-tidy',
    '--header-insertion=iwyu',
    '--query-driver=' .. os.getenv('HOME') .. '/.platformio/packages/*/bin/*',
  },
})
