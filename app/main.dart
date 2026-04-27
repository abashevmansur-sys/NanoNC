import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:web_socket_channel/web_socket_channel.dart';
import 'package:flutter_svg/flutter_svg.dart';
import 'package:wakelock_plus/wakelock_plus.dart';

void main() {
  runApp(const NanoNCApp());
}

class NanoNCApp extends StatelessWidget {
  const NanoNCApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark().copyWith(
        scaffoldBackgroundColor: const Color(0xFF050505),
      ),
      home: const ControlPage(),
    );
  }
}

class ControlPage extends StatefulWidget {
  const ControlPage({super.key});

  @override
  State<ControlPage> createState() => _ControlPageState();
}

class _ControlPageState extends State<ControlPage> {
  bool _isUpdatingFiles = false;
  bool _isStopPressed = false;
  final Color colorGreen = Color(0xFF4CD964);
  final Color colorBlue = Color(0xFF5AC8FA);
  final Color colorBorder = Color(0xFF3A3A3C);
  final Color colorPanel = const Color(0xFF1C1C1E);
  bool _isConnecting = false;
  String? selectedFile; // Файл, который просто выделен тапом
  final ScrollController _fileScrollController = ScrollController();

  WebSocketChannel? channel;
  String txStatus = "TX: Ready";
  String rxStatus = "RX: Offline";
  List<String> files = [];
  String? activeFile;
  String? pausedFile;
  Offset joyOffset = Offset.zero;
  double zThumbPercent = 0.5;
  bool isXY = false;
  bool isZ = false;
  bool _isZeroPressed = false; 
  double vx = 0, vy = 0, vz = 0;
  int vf = 0, vfz = 0;
  double lastVx = 0;
  double lastVy = 0;
  double lastVz = 0;
  Timer? sendTimer;

  final String rubySvg = '''
<svg viewBox="0 0 100 100">
  <defs>
    <linearGradient id="rubyBase" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" style="stop-color:#d60000" />
      <stop offset="100%" style="stop-color:#610000" />
    </linearGradient>
  </defs>
  <path d="M30,5 C10,5 5,10 5,30 L5,70 C5,90 10,95 30,95 L70,95 C90,95 95,90 95,70 L95,30 C95,10 90,5 70,5 Z" fill="#4d0000" />
  <path d="M30,5 C10,5 5,10 5,30 L5,70 C5,90 10,95 30,95 L70,95 C90,95 95,90 95,70 L95,30 C95,10 90,5 70,5 Z" fill="url(#rubyBase)" fill-opacity="0.95" />
  <path d="M30,5 C10,5 5,10 5,30 L5,70 C5,90 10,95 30,95 L70,95 C90,95 95,90 95,70 L95,30 C95,10 90,5 70,5 Z" fill="none" stroke="#ff4d4d" stroke-width="1.2" stroke-opacity="0.8" />
  <path d="M35,15 C20,15 15,20 15,35 L15,65 C15,80 20,85 35,85 L65,85 C80,85 85,80 85,65 L85,35 C85,20 80,15 65,15 Z" fill="none" stroke="white" stroke-width="1.8" stroke-opacity="0.25" />
  <path d="M30,12 C20,12 15,15 15,25 L15,45 C45,35 55,35 85,45 L85,25 C85,15 80,12 70,12 Z" fill="white" fill-opacity="0.5" />
  <rect x="30" y="15" width="40" height="2.5" rx="1" fill="white" fill-opacity="0.6" />
  <ellipse cx="50" cy="85" rx="30" ry="6" fill="white" fill-opacity="0.25" />
</svg>''';

  @override
  void initState() {
    super.initState();
    WakelockPlus.enable();
    connect();
    sendTimer = Timer.periodic(const Duration(milliseconds: 150), (timer) {
      // Добавьте проверку: если канала нет или он в ошибке — ничего не делаем
      if (channel == null) return; 

      // Логика XY
      if (isXY && (vx != lastVx || vy != lastVy)) {
        sendCommand("G1 X${(vx * 10).round()} Y${(vy * 10).round()} F${vf * 10}");
        lastVx = vx;
        lastVy = vy;
      }
      
      // Логика Z
      if (isZ && vz != lastVz) {
        int zSpeed = (vfz * 5).clamp(50, 500);
        sendCommand("G1 Z${(vz * 10).round()} F$zSpeed");
        lastVz = vz;
      }
    });
  }

  void connect() async {
    if (_isConnecting) return;
    _isConnecting = true;

    setState(() => rxStatus = "RX: Connecting...");

    try {
      final wsUri = Uri.parse('ws://192.168.4.1/ws');
      channel = WebSocketChannel.connect(wsUri);

      // Устанавливаем жесткий таймаут ожидания соединения 3 секунды
      // Если сервер не ответит за это время, уйдем в catch и попробуем снова быстро
      await channel!.ready.timeout(const Duration(seconds: 1));

      channel!.stream.listen(
        (message) => _handleMessage(message),
        onDone: () => _reconnect(),
        onError: (_) => _reconnect(),
        cancelOnError: true,
      );

      if (mounted) {
        setState(() {
          rxStatus = "RX: Connected";
          _isConnecting = false;
        });
        sendCommand('LIST_FILES');
      }
    } catch (e) {
      // Сюда мы попадем либо по ошибке, либо через 3 секунды по таймауту
      debugPrint("WS Error: $e");
      _reconnect();
    }
  }

  void _reconnect() {
    if (!_isConnecting && rxStatus == "RX: Offline") return;
    
    _isConnecting = false;
    if (mounted) {
      setState(() => rxStatus = "RX: Offline");
    }
    
    channel?.sink.close();
    channel = null;
    // Пауза перед следующей попыткой — 2 секунды
    Future.delayed(const Duration(seconds: 2), () {
      if (mounted) connect();
    });
  }

  void _handleMessage(dynamic msg) {
    String data = msg.toString();
    setState(() {
      if (data.startsWith("RX:")) {
        rxStatus = data;
      } else if (data.startsWith("TX:")) {
        txStatus = data;
        // Проверяем наличие "Done" в самом сообщении
        if (data.contains("Done")) {
          activeFile = null;
          pausedFile = null;
          // Обновляем текст статуса, как вы хотели ранее
          txStatus = "TX: Обработка завершена!";
          // Вызываем метод отправки команды (у вас он называется sendCommand)
          sendCommand('LIST_FILES');
        }
      } else {
        try {
          final jsonData = jsonDecode(data);
          
          if (jsonData['type'] == 'files') {
            files = List<String>.from(jsonData['list']);
            _isUpdatingFiles = false;
            
            // Проверка на завершение обработки
            if (jsonData['activeFile'] == "Done") {
              activeFile = null;
              pausedFile = null;
              txStatus = "TX: Обработка завершена!"; // Обновляем статус
              sendCommand('LIST_FILES'); // Обновляем список файлов после завершения
            } else if (jsonData['activeFile'] != "") {
              activeFile = jsonData['activeFile'];
            }
          }
        } catch (_) {}
      }
    });
  }

  void sendCommand(String cmd) {
    if (cmd != "LIST_FILES") {
      setState(() => txStatus = "TX: $cmd");
    }
    
    try {
      // Проверяем, что канал существует. 
      // Поскольку мы используем WebSocketChannel, sink не должен быть закрыт.
      channel?.sink.add(cmd);
    } catch (e) {
      debugPrint("Не удалось отправить команду: $e");
      // Если произошла ошибка записи, возможно, стоит инициировать переподключение
      _reconnect();
    }
  }

  void _handleRefresh() {
    setState(() {
      _isUpdatingFiles = true;
      files = []; 
    });

    connect(); 
    sendCommand('LIST_FILES');

    // Сбрасываем флаг обновления через 3 секунды для надежности, 
    // если вдруг ESP не ответит списком файлов
    Future.delayed(const Duration(seconds: 3), () {
      if (mounted) {
        setState(() => _isUpdatingFiles = false);
      }
    });
  }

  @override
  void dispose() {
    WakelockPlus.disable();
    _fileScrollController.dispose();
    sendTimer?.cancel();
    channel?.sink.close();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final bool isLocked = activeFile != null;
    return Scaffold(
      body: SafeArea(
        child: Stack(
          children: [
            // 1. Основной слой с контентом
            Column(
              children: [
                Padding(
                  padding: const EdgeInsets.fromLTRB(15, 15, 15, 0),
                  child: Row(
                    children: [
                      // Текст теперь в Flexible, он никогда не наедет на кнопку
                      Flexible(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(txStatus, 
                                style: const TextStyle(color: Color(0xFF4CD964), fontSize: 15, fontFamily: 'monospace'),
                                overflow: TextOverflow.ellipsis, maxLines: 1),
                            Text(rxStatus, 
                                style: TextStyle(color: const Color(0xFF5AC8FA).withAlpha(204), fontSize: 15, fontFamily: 'monospace'),
                                overflow: TextOverflow.ellipsis, maxLines: 1),
                          ],
                        ),
                      ),
                      const SizedBox(width: 60), // Отступ для кнопки Zero
                    ],
                  ),
                ),
                _buildZSlider(isLocked),
                Expanded(child: Center(child: _buildJoystick(isLocked))),
                _buildFileList(isLocked),
                Padding(
                  padding: const EdgeInsets.fromLTRB(15, 0, 15, 20),
                  child: GestureDetector(
                    onTapDown: (_) => setState(() => _isStopPressed = true),
                    onTapUp: (_) => setState(() => _isStopPressed = false),
                    onTapCancel: () => setState(() => _isStopPressed = false),
                    onTap: () {
                      HapticFeedback.vibrate();
                      setState(() { activeFile = null; pausedFile = null; });
                      sendCommand('STOP');
                    },
                    child: AnimatedScale(
                      scale: _isStopPressed ? 0.96 : 1.0,
                      duration: const Duration(milliseconds: 100),
                      child: AnimatedContainer(
                        duration: const Duration(milliseconds: 100),
                        width: double.infinity,
                        height: 60,
                        decoration: BoxDecoration(
                          color: _isStopPressed ? const Color(0xFFE00000) : const Color(0xFFB50000),
                          borderRadius: BorderRadius.circular(16),
                        ),
                        child: const Center(
                          child: Text("СТОП", style: TextStyle(fontSize: 22, fontWeight: FontWeight.w900, color: Colors.white, letterSpacing: 2)),
                        ),
                      ),
                    ),
                  ),
                ),
              ],
            ),

            // 2. Слой кнопки Zero (позиционирован поверх всего)
            Positioned(
              top: 15,
              right: 15,
              child: _buildZeroButton(isLocked),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildZeroButton(bool locked) {
  return RepaintBoundary(
    child: Listener(
      // Игнорируем нажатия, если locked
      onPointerDown: locked ? null : (_) {
        setState(() => _isZeroPressed = true);
        HapticFeedback.mediumImpact();
      },
      onPointerUp: locked ? null : (_) {
        setState(() => _isZeroPressed = false);
        sendCommand('G92 X0 Y0 Z0');
      },
      onPointerCancel: locked ? null : (_) {
        setState(() => _isZeroPressed = false);
      },
      child: AnimatedScale(
        duration: const Duration(milliseconds: 100),
        scale: _isZeroPressed ? 0.85 : 1.0,
        child: AnimatedOpacity(
          duration: const Duration(milliseconds: 100),
          opacity: locked ? 0.3 : 1.0,
          child: Container(
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              color: (_isZeroPressed && !locked) ? colorGreen.withAlpha(30) : Colors.transparent,
              border: Border.all(color: colorGreen, width: 1),
              borderRadius: BorderRadius.circular(12),
            ),
            child: Center(
              child: SvgPicture.string('''
                <svg viewBox="0 0 100 100">
                  <circle cx="50" cy="50" r="35" stroke="#4CD964" stroke-width="4" fill="none"/>
                  <line x1="50" y1="5" x2="50" y2="95" stroke="#4CD964" stroke-width="4"/>
                  <line x1="5" y1="50" x2="95" y2="50" stroke="#4CD964" stroke-width="4"/>
                </svg>''', width: 24),
            ),
          ),
        ),
      ),
    ),
  );
}
  
  Widget _buildZSlider(bool locked) {
    return IgnorePointer(
      ignoring: locked,
      child: Opacity(
        opacity: locked ? 0.3 : 1.0,
        child: Container(
          height: 80,
          width: double.infinity,
          margin: const EdgeInsets.symmetric(vertical: 10),
          color: Colors.transparent,
          child: LayoutBuilder(builder: (context, constraints) {
            double trackW = constraints.maxWidth * 0.85;
            double sideMargin = (constraints.maxWidth - trackW) / 2;

            return Listener(
              behavior: HitTestBehavior.opaque,
              onPointerDown: (e) {
                setState(() => isZ = true);
              },
              onPointerMove: (e) {
                if (!isZ) return;
                double localX = e.localPosition.dx - sideMargin;
                double percent = (localX / trackW).clamp(0.0, 1.0);

                setState(() {
                  zThumbPercent = percent;
                  double rawZ = (zThumbPercent - 0.5) * 2;
                  vz = double.parse(rawZ.toStringAsFixed(2));
                  vfz = (vz.abs() < 0.1) ? 0 : (vz.abs() * 100).round();
                  if (vz.abs() < 0.1) vz = 0;
                });
                int zSpeed = (vfz * 5).clamp(50, 500);
                sendCommand("G1 Z${(vz * 10).round()} F$zSpeed");
              },
              onPointerUp: (e) {
                setState(() {
                  isZ = false;
                  zThumbPercent = 0.5;
                  vz = 0;
                  vfz = 0;
                  sendCommand('STOP');
                });
              },
              child: Stack(
                alignment: Alignment.center,
                children: [
                  // Линия трека
                  Container(
                    width: trackW,
                    height: 1.3,
                    decoration: const BoxDecoration(
                      gradient: LinearGradient(
                        colors: [Colors.transparent, Colors.white, Colors.transparent],
                      ),
                    ),
                  ),
                  // Бегунок с цветофильтром
                  ColorFiltered(
                    colorFilter: locked
                        ? const ColorFilter.matrix(<double>[
                            0.2126, 0.7152, 0.0722, 0, 0, // Серые тона
                            0.2126, 0.7152, 0.0722, 0, 0,
                            0.2126, 0.7152, 0.0722, 0, 0,
                            0,      0,      0,      1, 0,
                          ])
                        : const ColorFilter.mode(Colors.transparent, BlendMode.dst),
                    child: AnimatedContainer(
                      duration: Duration(milliseconds: isZ ? 0 : 250),
                      curve: Curves.elasticOut,
                      transform: Matrix4.translationValues((zThumbPercent - 0.5) * trackW, 0, 0),
                      width: 50,
                      height: 50,
                      child: SvgPicture.string(rubySvg),
                    ),
                  ),
                ],
              ),
            );
          }),
        ),
      ),
    );
  }

  Widget _buildJoystick(bool locked) {
    double screenWidth = MediaQuery.of(context).size.width;
    double size = screenWidth * 0.7;

    return IgnorePointer(
      ignoring: locked,
      child: Opacity(
        // Применяем прозрачность ко всему объекту, как в Z-ползунке
        opacity: locked ? 0.3 : 1.0,
        child: Listener(
          onPointerDown: (e) {
            setState(() => isXY = true);
          },
          onPointerMove: (e) {
            if (!isXY) return;
            double max = (size / 2) - 30;
            Offset delta = e.localPosition - Offset(size / 2, size / 2);
            if (delta.distance > max) {
              delta = Offset.fromDirection(delta.direction, max);
            }
            
            double stickThreshold = 12.0;
            double dx = delta.dx;
            double dy = delta.dy;
            if (dx.abs() < stickThreshold) dx = 0;
            if (dy.abs() < stickThreshold) dy = 0;
            
            Offset snappedDelta = Offset(dx, dy);
            setState(() {
              joyOffset = snappedDelta;
              vx = double.parse((snappedDelta.dx / max).toStringAsFixed(2));
              vy = double.parse((-snappedDelta.dy / max).toStringAsFixed(2));
              vf = ((snappedDelta.distance / max) * 100).round();
              if (vx.abs() < 0.1) vx = 0;
              if (vy.abs() < 0.1) vy = 0;
            });
          },
          onPointerUp: (e) {
            setState(() {
              isXY = false;
              joyOffset = Offset.zero;
              vx = 0;
              vy = 0;
              sendCommand('STOP');
            });
          },
          child: SizedBox(
            width: size,
            height: size,
            child: Stack(
              alignment: Alignment.center,
              children: [
                // 1. Линии трека (внизу)
                Container(
                  width: size,
                  height: 1.2,
                  decoration: const BoxDecoration(
                    gradient: LinearGradient(
                      colors: [Color.fromARGB(20, 255, 255, 255), Colors.white, Color.fromARGB(20, 255, 255, 255)],
                    ),
                  ),
                ),
                Container(
                  width: 1.2,
                  height: size,
                  decoration: const BoxDecoration(
                    gradient: LinearGradient(
                      begin: Alignment.topCenter,
                      end: Alignment.bottomCenter,
                      colors: [Color.fromARGB(20, 255, 255, 255), Colors.white, Color.fromARGB(20, 255, 255, 255)],
                    ),
                  ),
                ),
                // 2. Джойстик (сверху)
                ColorFiltered(
                  colorFilter: locked
                      ? const ColorFilter.matrix(<double>[
                          0.2126, 0.7152, 0.0722, 0, 0, // Серые тона
                          0.2126, 0.7152, 0.0722, 0, 0,
                          0.2126, 0.7152, 0.0722, 0, 0,
                          0,      0,      0,      1, 0,
                        ])
                      : const ColorFilter.mode(Colors.transparent, BlendMode.dst),
                  child: AnimatedContainer(
                    duration: Duration(milliseconds: isXY ? 0 : 250),
                    curve: Curves.elasticOut,
                    transform: Matrix4.translationValues(joyOffset.dx, joyOffset.dy, 0),
                    width: 80,
                    height: 80,
                    child: SvgPicture.string(rubySvg),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildFileList(bool locked) {
    return Container(
      width: double.infinity,
      height: 180,
      margin: const EdgeInsets.symmetric(horizontal: 15, vertical: 10),
      decoration: BoxDecoration(
        color: colorPanel,
        borderRadius: BorderRadius.circular(16), // Твое скругление
        border: Border.all(color: colorBorder),
      ),
      // Добавляем ClipRRect, чтобы содержимое (включая черту) не вылезало за углы
      child: ClipRRect(
        borderRadius: BorderRadius.circular(16),
        child: files.isEmpty ? _buildEmptyState() : _buildList(locked),
      ),
    );
  }

  Widget _buildEmptyState() {
    return Column(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        Text(
          _isUpdatingFiles ? "ОБНОВЛЕНИЕ..." : "ФАЙЛЫ НЕ НАЙДЕНЫ",
          style: TextStyle( // Удалил const отсюда
            color: colorBlue,
            fontWeight: FontWeight.bold,
            fontSize: 16,
            fontFamily: 'monospace',
          ),
        ),
        if (!_isUpdatingFiles) ...[
          const SizedBox(height: 4),
          const Text(
            "ЗАГРУЗИТЕ G-CODE НА SD КАРТУ",
            style: TextStyle(color: Colors.white38, fontSize: 10, fontFamily: 'monospace'),
          ),
          const SizedBox(height: 15),
          ElevatedButton.icon(
            style: ElevatedButton.styleFrom(
              backgroundColor: colorBlue.withValues(alpha: 0.1), // Исправлено
              foregroundColor: colorBlue,
              side: BorderSide(color: colorBlue, width: 1),
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
              elevation: 0,
            ),
            onPressed: _handleRefresh,
            icon: const Icon(Icons.refresh, size: 18),
            label: const Text("ОБНОВИТЬ"),
          ),
        ],
      ],
    );
  }

  Widget _buildList(bool locked) {
    return RawScrollbar(
      controller: _fileScrollController,
      thumbColor: colorBlue.withValues(alpha: 0.4),
      radius: const Radius.circular(10),
      thickness: 3,
      thumbVisibility: true,
      interactive: true,
      child: ListView.builder(
        controller: _fileScrollController,
        padding: EdgeInsets.zero,
        itemCount: files.length,
        // Быстрая физика скролла без ошибок компиляции
        physics: const BouncingScrollPhysics(
          decelerationRate: ScrollDecelerationRate.fast,
        ),
        itemBuilder: (context, index) {
          String f = files[index];
          bool isRunning = activeFile == f;
          bool isSelected = selectedFile == f;
          bool isActiveInMachine = activeFile == f || pausedFile == f;
          bool isDisabled = (activeFile != null || pausedFile != null) && !isActiveInMachine;

          return Opacity(
            opacity: isDisabled ? 0.3 : 1.0,
            child: AnimatedContainer(
              duration: const Duration(milliseconds: 80), // Шустрая анимация выбора
              decoration: BoxDecoration(
                color: isSelected ? colorBlue.withValues(alpha: 0.08) : Colors.transparent,
                border: Border(
                  bottom: BorderSide(color: colorBorder, width: 0.5),
                  left: BorderSide(
                    color: isSelected ? colorBlue : Colors.transparent,
                    width: 4,
                  ),
                ),
              ),
              child: Theme(
                data: Theme.of(context).copyWith(
                  splashColor: Colors.transparent,
                  highlightColor: Colors.transparent,
                ),
                child: ListTile(
                  dense: true,
                  onTap: isDisabled ? null : () {
                    setState(() {
                      selectedFile = f;
                    });
                  },
                  contentPadding: const EdgeInsets.symmetric(horizontal: 16),
                  title: Text(
                    f,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      fontFamily: 'monospace',
                      fontSize: 16,
                      color: isSelected ? colorBlue : const Color(0xFFEFEFF4),
                      fontWeight: isSelected ? FontWeight.bold : FontWeight.normal,
                    ),
                  ),
                  // Кнопка Пуск/Пауза в стиле HTML (Квадрат с обводкой)
                  trailing: GestureDetector(
                    onTap: isDisabled ? null : () {
                      setState(() {
                        selectedFile = f;
                        if (isRunning) {
                          activeFile = null;
                          pausedFile = f;
                          sendCommand('PAUSE');
                        } else {
                          activeFile = f;
                          pausedFile = null;
                          sendCommand('START $f');
                        }
                      });
                    },
                    child: AnimatedContainer(
                      duration: const Duration(milliseconds: 150),
                      width: 36,
                      height: 36,
                      decoration: BoxDecoration(
                        color: isRunning ? colorGreen.withValues(alpha: 0.1) : Colors.transparent,
                        border: Border.all(
                          color: isRunning ? colorGreen : colorGreen.withValues(alpha: 0.4),
                          width: 1.3,
                        ),
                        borderRadius: BorderRadius.circular(8),
                      ),
                      child: Center(
                        child: Icon(
                          isRunning ? Icons.pause : Icons.play_arrow,
                          color: colorGreen,
                          size: 22,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          );
        },
      ),
    );
  }



}