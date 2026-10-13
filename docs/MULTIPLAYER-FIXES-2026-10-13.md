# Correções de posição e autoridade — 13/10/2026

As alterações corrigem incompatibilidades entre o cliente real, o servidor e os clientes de teste.
A base examinada foi o commit db743a19eca177ad04ddaac1cb92891c1e2942a4 de Tobidp/Kenshi-Online.
Os exemplos de “depois” abaixo correspondem ao código alterado nesta entrega.

## Escopo e validação

| Item | Situação |
| --- | --- |
| Revisão do CONTRIBUTING.md e aplicação do Code Style Guide | Realizado |
| Leitura da implementação e comparação com protocolo/testes | Realizado |
| Conferência das cinco dependências nos respectivos commits upstream | Realizado |
| Demonstração binária independente: 24 bytes antes, 28 bytes com generation | Executada em PowerShell |
| Revisão estática dos serializers, readers, autorização e listas CMake | Realizada |
| Regressões C++ de serialização, autoridade e posições recebidas | Adicionadas; execução pendente |
| Compilação Release x64 | Pendente: Visual Studio/CMake ausentes neste ambiente |
| IntegrationTest com servidor compilado | Pendente |
| Teste dentro do Kenshi com dois clientes e depois 16 | Pendente |

Nenhuma DLL/EXE distribuída foi recompilada nesta entrega. As alterações são de fonte e documentação.
O atendimento ao estilo inclui quatro espaços, métodos PascalCase, variáveis camelCase,
constantes SCREAMING_SNAKE_CASE, namespace kmp e comentários que explicam as decisões.

## 1. Um formato de posição compartilhado

### Antes

O Core e StagePollAndSendPositions escreviam os campos manualmente, omitindo generation:

~~~cpp
writer.WriteU32(netId);
writer.WriteF32(pos.x);
writer.WriteF32(pos.y);
writer.WriteF32(pos.z);
writer.WriteU32(compQuat);
writer.WriteU8(animState);
writer.WriteU8(moveSpeedU8);
writer.WriteU16(flags);
~~~

O servidor lia sizeof(CharacterPosition), equivalente a 28 bytes, mas esse trecho produzia 24.
Para a mensagem normal de um personagem, a leitura falhava antes de atualizar a posição.

### Depois

Os emissores preenchem a estrutura inteira e usam WriteCharacterPosition, definido em Common:

~~~cpp
CharacterPosition position{};
position.entityId = netId;
position.generation = infoCopy->generation;
position.posX = pos.x;
position.posY = pos.y;
position.posZ = pos.z;
position.compressedQuat = compQuat;
position.animStateId = animState;
position.moveSpeed = moveSpeedU8;
position.flags = flags;
WriteCharacterPosition(writer, position);
~~~

O helper serializa todos os campos, na ordem do protocolo, com 28 bytes. ReadCharacterPosition
faz a leitura correspondente e não consome registros truncados. Um static_assert protege o
tamanho declarado da estrutura. Os caminhos de background também inicializam generation.

Os clientes de integração e TestClient usam o mesmo serializer da produção. O protocolo
subiu para 2: cliente e servidor precisam ser recompilados juntos. O handshake rejeita pares
que ainda anunciam protocolo 1, incluindo binários antigos de dist.

## 2. Arquivos de build e dependências

### Antes

Os targets não incluíam as implementações dos validators e das filas novas.
As bibliotecas estavam fixadas como gitlinks, mas faltava .gitmodules.

### Depois

No Core/CMakeLists.txt foram acrescentadas:

~~~cmake
sync/authority_validator.cpp
sync/pending_snapshot_queue.cpp
sync/deferred_spawn_queue.cpp
~~~

No Server/CMakeLists.txt:

~~~cmake
authority_validator.cpp
~~~

.gitmodules agora mapeia ENet, ImGui, JSON, MinHook e spdlog aos respectivos repositórios
canônicos. Os cinco commits fixados existentes foram verificados e preservados.
pending_snapshot_queue.cpp inclui algorithm explicitamente para usar remove_if.

Para preparar o build:

~~~powershell
git submodule update --init --recursive
cmake -S . -B build -A x64
cmake --build build --config Release
~~~

Essas mudanças resolvem as omissões identificadas; o build completo ainda precisa ser executado.

## 3. Autoridade coerente no personagem

### Antes

O spawn atribuía o proprietário, mas mantinha a autoridade padrão Server:

~~~cpp
entity.owner = player.id;
// entity.authority continuava AuthorityType::Server.
~~~

CanClientCommandEntity exige AuthorityType::Player, então rejeitava o comando do próprio dono.

### Depois

~~~cpp
entity.owner = player.id;
entity.authority = entity.type == EntityType::PlayerCharacter
    ? AuthorityType::Player : AuthorityType::Server;
~~~

A mesma classificação é aplicada ao retomar entidades na reconexão e ao carregar saves antigos.
NPCs e objetos do mundo continuam com autoridade Server; a política para esquadrões classificados
como NPC precisa de implementação própria.

O servidor valida a geração junto com a autoridade e o proprietário:

~~~cpp
if (!ServerAuthorityValidator::ValidatePositionUpdate(
        player.id, pos.entityId, pos.generation, m_entities)) {
    continue;
}
~~~

No cliente, a comparação entre sourcePlayer e owner acontece antes de ReconcileLocal. Um snapshot
atribuído a outro jogador é rejeitado também quando tenta representar uma entidade local.
O callback de rede só recebe pacotes do peer esperado do servidor.

## 4. Posições distribuídas por proprietário

### Antes

~~~cpp
writer.WriteU32(0); // sourcePlayer = server
~~~

O pacote reunia entidades de vários jogadores, enquanto o cliente exigia sourcePlayer==owner.
Além disso, apenas as primeiras 255 entidades eram enviadas, sem paginação.

### Depois

Cada lote contém entidades de um único proprietário:

~~~cpp
writer.WriteHeader(MessageType::S2C_PositionUpdate, 0, m_serverTick);
writer.WriteU32(sourcePlayer);
writer.WriteU8(static_cast<uint8_t>(batch.entityCount));
~~~

sourcePlayer vem de entity.owner, mantido no servidor. Entidades do destinatário são excluídas.
Todos os lotes são percorridos; o tamanho respeita tanto o contador de oito bits quanto a MTU
do peer. O canal sequenciado e as flags 0 são preservados. Falhas de envio são registradas e
o pacote é liberado.

A ordem dos lotes gira por tick, reduzindo o favorecimento permanente de um prefixo em caso de
reordenação. Pacotes ainda podem ser perdidos em um tick; retransmissão confiável de cada posição
não é prometida. O próximo tick envia o estado novamente.

## 5. Save compartilhado usa referências e IDs reais

### Antes

~~~cpp
if (faction == "10-kenshi-online") return "Player 1";
// O servidor enviava "10-kenshi-online.mod".

cp.entityId = 0; // Avatar sem entidade registrada no servidor.
~~~

### Depois

As comparações usam as mesmas referências com .mod que o servidor envia. A posição só é enviada
quando o personagem tem registro local com o proprietário correto:

~~~cpp
const EntityID entityId = core.GetEntityRegistry().GetNetId(s_ownCharPtr);
const auto entityInfo = core.GetEntityRegistry().GetInfo(entityId);
if (entityInfo && entityInfo->ownerPlayerId == core.GetLocalPlayerId() &&
    !entityInfo->isRemote && SEH_ReadAnimClassPosition(s_ownAnimClass, myPos)) {
    // Preencher posição, rotação, animação e velocidade do registro.
    // cp.entityId = entityId;
    // cp.generation = entityInfo->generation;
}
~~~

Isso evita enviar um avatar inválido e preserva a geração/rotação disponíveis no registro.
O shared-save continua projetado para dois personagens. As alterações não criam os 16 slots
independentes que ainda faltam.

## 6. Testes exigem o estado recebido

### Antes

~~~cpp
case MessageType::S2C_PositionUpdate: {
    posUpdatesReceived++;
    break;
}
~~~

O teste aprovava movimento apenas pelo aumento desse contador, mesmo com coordenadas antigas.

### Depois

O cliente de integração lê os registros e captura sourcePlayer, entityId, generation e posição.
A aprovação procura as coordenadas finais efetivamente enviadas, nos dois sentidos:

~~~cpp
client2.HasPositionSnapshot(client1.playerId, client1.myEntityId, 0,
                           -51160.f, 1600.f, 2700.f);
client1.HasPositionSnapshot(client2.playerId, client2.myEntityId, 0,
                           -51180.f, 1600.f, 2760.f);
~~~

O teste de sessão completa também confere coordenadas. Outro cenário usa mais de 255 entidades
e verifica recepção de diferentes páginas/proprietários. Os testes fazem flush dos envios e
polling dos dois clientes para que um envio pendente não seja confundido com rejeição.

UnitTest liga os validators reais do cliente e servidor. As regressões incluem formato de
28 bytes, geração não zero, registros consecutivos/truncados, dono correto/incorreto, entidade
desconhecida/morta, autoridade Server/Transferring e origem forjada antes da reconciliação.

Esses testes foram acrescentados ao código, mas não executados neste ambiente.
Mesmo quando passarem, receber um payload correto ainda precisa ser complementado com
verificação do movimento de um personagem dentro do Kenshi.

## O que continua pendente

- Build Release x64, UnitTest e IntegrationTest, seguidos dos testes manuais previstos no CONTRIBUTING.md.
- Dois clientes reais com movimento bidirecional, late join, reconexão e troca de zona.
- Slots/facções independentes e estado remoto por jogador para validar 4, 8 e 16 participantes.
- Ciclo completo de geração: alocação/reutilização, spawn/despawn e persistência.
- Alinhar relevância entre posições e heartbeat.
- Esquadrões completos, dano/IA, inventário e construções físicas remotas.
- Revisão dos buffers de background para limites de contagem/MTU em esquadrões grandes.

A alteração fecha inconsistências concretas do fluxo de posição e acrescenta regressões para
detectá-las. A estabilidade do multiplayer exige a validação de runtime descrita acima.

