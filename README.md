![GitHub all releases](https://img.shields.io/github/downloads/ABKAM2023/Anti-Afk/total?style=for-the-badge)

**Anti-Afk** - автоматически убивает, кикает или переводит в наблюдатели игроков, находящихся в AFK.

## Требования
[Utils](https://github.com/Pisex/cs2-menus/releases)

## Конфиг
```ini
"AntiAfk"
{
    // Время (в секундах), через которое игроку отправляется первое предупреждение об AFK.
    "initial_warning_time" "15"
    
    // Общее время (в секундах), по истечении которого производится действие (убийство/кик/перевод в наблюдатели).
    "warning_interval" "35"
    
    // Максимальное количество предупреждений до применения финальной меры.
    "max_warnings" "3"
    
    // Если установлено в "1", то вместо перевода в наблюдатели игрок будет кикнут.
    "kick_instead_of_spec" "0"
}
```
