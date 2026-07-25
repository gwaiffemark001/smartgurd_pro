import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

void main() => runApp(const SmartGuardApp());

class SmartGuardApp extends StatelessWidget {
  const SmartGuardApp({super.key});
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'SmartGuard Pro',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        brightness: Brightness.dark,
        scaffoldBackgroundColor: const Color(0xFF0D1117),
        colorScheme: const ColorScheme.dark(
          primary: Color(0xFF00E676),
          secondary: Color(0xFFFF9100),
          error: Color(0xFFFF1744),
        ),
      ),
      home: const DashboardScreen(),
    );
  }
}

class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});
  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  MqttServerClient? client;
  String connectionStatus = 'Disconnected';
  String currentStatus = 'SAFE';
  int gasLevel = 0;
  int flameLevel = 0;
  List<Map<String, dynamic>> historyLog = [];
  bool isAlarmActive = false;

  // Match Arduino's broker list exactly
  final List<String> mqtt_servers = [
    'test.mosquitto.org',       // PRIMARY (matches Arduino)
    'broker.emqx.io',           // FALLBACK 1
    'broker.hivemq.com',        // FALLBACK 2
    'mqtt.eclipseprojects.io'   // FALLBACK 3
  ];
  int current_broker_index = 0;
  final int mqtt_port = 1883;
  
  bool _isConnecting = false;

  @override
  void initState() {
    super.initState();
    _connectToMqtt();
  }

  void _connectToMqtt() {
    if (_isConnecting) return;
    _isConnecting = true;
    
    final clientId = 'flutter_smartguard_${DateTime.now().millisecondsSinceEpoch}';
    final broker = mqtt_servers[current_broker_index];
    
    print('Attempting MQTT connection to $broker');
    
    client = MqttServerClient(broker, clientId);
    client!.port = mqtt_port;
    client!.keepAlivePeriod = 60;
    client!.logging(on: true);
    client!.onConnected = onConnected;
    client!.onDisconnected = onDisconnected;

    final connMessage = MqttConnectMessage()
        .withClientIdentifier(clientId)
        .startClean()
        .withWillQos(MqttQos.atLeastOnce);
    client!.connectionMessage = connMessage;

    connect();
  }

  void connect() async {
    setState(() => connectionStatus = 'Connecting...');
    try {
      await client!.connect();
    } catch (e) {
      print('failed, rc=${client!.connectionStatus!.state} switching broker...');
      _switchBrokerAndRetry();
      return;
    }

    if (client!.connectionStatus!.state == MqttConnectionState.connected) {
      print('connected!');
      setState(() => connectionStatus = 'Connected');
      client!.subscribe('smartguard/alerts', MqttQos.atLeastOnce);
      _isConnecting = false;
      
      client!.updates!.listen((List<MqttReceivedMessage<MqttMessage>> c) {
        final MqttPublishMessage message = c[0].payload as MqttPublishMessage;
        final payload = MqttPublishPayload.bytesToStringAsString(message.payload.message);
        print('Published MQTT: $payload');
        _handleIncomingData(payload);
      });
    } else {
      print('failed, rc=${client!.connectionStatus!.state} switching broker...');
      _switchBrokerAndRetry();
    }
  }

  void _switchBrokerAndRetry() {
    // Cycle to next broker (exactly like Arduino: (current + 1) % NUM_BROKERS)
    current_broker_index = (current_broker_index + 1) % mqtt_servers.length;
    _isConnecting = false;
    
    // Wait 5 seconds before trying next broker (matches Arduino delay)
    Future.delayed(const Duration(seconds: 5), () {
      if (mounted) {
        _connectToMqtt();
      }
    });
  }

  void _handleIncomingData(String payload) {
    try {
      final data = jsonDecode(payload);
      final String newStatus = data['status'] ?? 'UNKNOWN';
      final int newGas = data['gas'] ?? 0;
      final int newFlame = data['flame'] ?? 0;

      setState(() {
        currentStatus = newStatus;
        gasLevel = newGas;
        flameLevel = newFlame;
        isAlarmActive = (newStatus != 'SAFE' && newStatus != 'SILENCED');

        if (newStatus != 'SAFE' && newStatus != 'SILENCED') {
          final time = DateTime.now().toString().substring(11, 19);
          historyLog.insert(0, {'time': time, 'status': newStatus, 'gas': newGas, 'flame': newFlame});
          if (historyLog.length > 15) historyLog.removeLast();
        }
      });
    } catch (e) {
      print('Error parsing JSON: $e');
    }
  }

  void _sendCommand(String command) {
    if (client != null && client!.connectionStatus!.state == MqttConnectionState.connected) {
      final builder = MqttClientPayloadBuilder();
      builder.addString('{"command": "$command"}');
      client!.publishMessage('smartguard/commands', MqttQos.atLeastOnce, builder.payload!);
      
      if (command == 'silence_buzzer') {
        setState(() {
          isAlarmActive = false;
          currentStatus = 'SILENCED';
        });
      }
    }
  }

  void onConnected() {
    setState(() => connectionStatus = 'Connected');
    // Reset to primary broker (index 0) for future reconnects - matches Arduino
    current_broker_index = 0;
    _isConnecting = false;
  }
  
  void onDisconnected() {
    setState(() => connectionStatus = 'Disconnected');
  }

  Color _getStatusColor() {
    switch (currentStatus) {
      case 'SAFE': return const Color(0xFF00E676);
      case 'GAS/SMOKE': return const Color(0xFFFF9100);
      case 'FIRE': return const Color(0xFFFF1744);
      case 'SHORT CIRCUIT': return const Color(0xFFD500F9);
      case 'SILENCED': return const Color(0xFF607D8B);
      default: return Colors.grey;
    }
  }

  @override
  Widget build(BuildContext context) {
    final statusColor = _getStatusColor();

    return Scaffold(
      appBar: AppBar(
        backgroundColor: Colors.transparent,
        elevation: 0,
        title: const Text('SmartGuard Pro', style: TextStyle(fontWeight: FontWeight.bold, letterSpacing: 1.2)),
        actions: [
          IconButton(
            icon: Icon(
              Icons.refresh,
              color: connectionStatus == 'Connected' ? Colors.green : Colors.red,
            ),
            tooltip: 'Reconnect',
            onPressed: () {
              current_broker_index = 0; // Reset to primary on manual refresh
              _connectToMqtt();
            },
          ),
          Padding(
            padding: const EdgeInsets.only(right: 12.0),
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
              decoration: BoxDecoration(
                color: connectionStatus == 'Connected' ? Colors.green.withOpacity(0.2) : Colors.red.withOpacity(0.2),
                borderRadius: BorderRadius.circular(20),
                border: Border.all(color: connectionStatus == 'Connected' ? Colors.green : Colors.red),
              ),
              child: Text(
                connectionStatus,
                style: TextStyle(fontSize: 12, fontWeight: FontWeight.bold, color: connectionStatus == 'Connected' ? Colors.green : Colors.red),
              ),
            ),
          ),
        ],
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(20.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            AnimatedContainer(
              duration: const Duration(milliseconds: 600),
              padding: const EdgeInsets.all(28),
              decoration: BoxDecoration(
                gradient: LinearGradient(
                  colors: [statusColor.withOpacity(0.1), statusColor.withOpacity(0.05)],
                  begin: Alignment.topLeft,
                  end: Alignment.bottomRight,
                ),
                borderRadius: BorderRadius.circular(24),
                border: Border.all(color: statusColor.withOpacity(0.4), width: 1.5),
                boxShadow: isAlarmActive 
                  ? [BoxShadow(color: statusColor.withOpacity(0.3), blurRadius: 20, spreadRadius: 2)] 
                  : [],
              ),
              child: Column(
                children: [
                  Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      if (isAlarmActive) ...[
                        const Icon(Icons.warning_amber_rounded, size: 32, color: Colors.white),
                        const SizedBox(width: 12),
                      ],
                      Text(currentStatus.replaceAll('_', ' '), 
                        style: TextStyle(fontSize: 38, fontWeight: FontWeight.w900, color: Colors.white, letterSpacing: 1.5, shadows: [
                          Shadow(color: statusColor, blurRadius: 10)
                        ])),
                    ],
                  ),
                  const SizedBox(height: 12),
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                    decoration: BoxDecoration(color: Colors.black26, borderRadius: BorderRadius.circular(12)),
                    child: Text('System is currently monitoring', style: TextStyle(color: Colors.grey[300], fontSize: 14, letterSpacing: 0.5)),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 28),
            Row(
              children: [
                Expanded(child: _PremiumGauge(title: 'GAS (MQ-2)', value: gasLevel, max: 1024, icon: Icons.air, color: Colors.blueAccent)),
                const SizedBox(width: 16),
                Expanded(child: _PremiumGauge(title: 'FLAME', value: flameLevel, max: 1024, icon: Icons.local_fire_department, color: Colors.orange)),
              ],
            ),
            const SizedBox(height: 28),
            AnimatedContainer(
              duration: const Duration(milliseconds: 300),
              child: ElevatedButton.icon(
                onPressed: isAlarmActive ? () => _sendCommand('silence_buzzer') : null,
                icon: const Icon(Icons.volume_off_rounded, size: 24),
                label: const Text('SILENCE ALARM REMOTELY', style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold, letterSpacing: 1)),
                style: ElevatedButton.styleFrom(
                  backgroundColor: isAlarmActive ? const Color(0xFFFF1744) : Colors.grey[800],
                  foregroundColor: Colors.white,
                  padding: const EdgeInsets.symmetric(vertical: 20),
                  shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
                  elevation: isAlarmActive ? 10 : 0,
                  shadowColor: isAlarmActive ? Colors.redAccent : Colors.transparent,
                ),
              ),
            ),
            const SizedBox(height: 32),
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                const Text('RECENT ACTIVITY', style: TextStyle(fontSize: 14, fontWeight: FontWeight.bold, letterSpacing: 1.5, color: Colors.grey)),
                if (historyLog.isNotEmpty) 
                  TextButton(onPressed: () => setState(() => historyLog.clear()), child: const Text('Clear', style: TextStyle(color: Colors.grey)))
              ],
            ),
            const SizedBox(height: 12),
            Container(
              height: 220,
              decoration: BoxDecoration(
                color: const Color(0xFF161B22),
                borderRadius: BorderRadius.circular(20),
                border: Border.all(color: Colors.grey[800]!),
              ),
              child: historyLog.isEmpty 
                ? const Center(child: Text('No recent alerts. System is safe.', style: TextStyle(color: Colors.grey, fontSize: 14)))
                : ListView.builder(
                    padding: const EdgeInsets.all(16),
                    itemCount: historyLog.length,
                    itemBuilder: (context, index) {
                      final log = historyLog[index];
                      return Padding(
                        padding: const EdgeInsets.symmetric(vertical: 8.0),
                        child: Row(
                          children: [
                            Container(
                              padding: const EdgeInsets.all(8),
                              decoration: BoxDecoration(color: Colors.grey[800], borderRadius: BorderRadius.circular(8)),
                              child: const Icon(Icons.history, size: 18, color: Colors.grey),
                            ),
                            const SizedBox(width: 16),
                            Expanded(
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Text(log['status']!, style: const TextStyle(fontSize: 15, fontWeight: FontWeight.bold, color: Colors.white)),
                                  const SizedBox(height: 4),
                                  Text('Gas: ${log['gas']} | Flame: ${log['flame']}', style: TextStyle(fontSize: 12, color: Colors.grey[500])),
                                ],
                              ),
                            ),
                            Text(log['time']!, style: TextStyle(fontSize: 12, color: Colors.grey[600], fontFamily: 'monospace')),
                          ],
                        ),
                      );
                    },
                  ),
            ),
          ],
        ),
      ),
    );
  }
}

class _PremiumGauge extends StatelessWidget {
  final String title;
  final int value;
  final int max;
  final IconData icon;
  final Color color;

  const _PremiumGauge({required this.title, required this.value, required this.max, required this.icon, required this.color});

  @override
  Widget build(BuildContext context) {
    final percentage = (value / max).clamp(0.0, 1.0);
    return Container(
      padding: const EdgeInsets.all(20),
      decoration: BoxDecoration(
        color: const Color(0xFF161B22),
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: Colors.grey[800]!),
      ),
      child: Column(
        children: [
          Icon(icon, size: 36, color: color),
          const SizedBox(height: 16),
          Text(title, style: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600, color: Colors.grey, letterSpacing: 1)),
          const SizedBox(height: 12),
          Text('$value', style: TextStyle(fontSize: 36, fontWeight: FontWeight.w900, color: color)),
          const SizedBox(height: 16),
          ClipRRect(
            borderRadius: BorderRadius.circular(10),
            child: LinearProgressIndicator(
              value: percentage,
              backgroundColor: Colors.grey[800],
              valueColor: AlwaysStoppedAnimation<Color>(color),
              minHeight: 10,
            ),
          ),
        ],
      ),
    );
  }
}
