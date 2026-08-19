" The demo's vim: syntax highlighting on a pruned runtime -- syntax/,
" filetype detection, colours, and nothing else. :help is not installed.
set nocompatible
filetype on
syntax on

" slosh's terminal does 24-bit colour (it is most of what the shaders are),
" and the pane's TERM is an alias of xterm-256color, whose terminfo does not
" say so. COLORTERM comes from /etc/profile; the t_8f/t_8b overrides are the
" standard incantation for truecolor on a TERM that is not *-direct.
if $COLORTERM ==# 'truecolor'
  let &t_8f = "\<Esc>[38;2;%lu;%lu;%lum"
  let &t_8b = "\<Esc>[48;2;%lu;%lu;%lum"
  set termguicolors
endif
colorscheme habamax

" A pane in an emulated machine: keep vim from wanting things that are not
" there. No swapfile chatter on a throwaway disk, no mouse fights with the
" multiplexer's own mouse handling.
set noswapfile
set mouse=
set laststatus=1
