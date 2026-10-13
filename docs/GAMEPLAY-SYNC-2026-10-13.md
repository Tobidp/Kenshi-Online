# Vida, reconciliação e NPCs — 13/10/2026

Esta etapa usa como base c88da507c37a682b2fd634307cc75c248327a25b, na branch de correção existente.
Implementa caminhos de fonte para vida e reconciliação. A sincronização completa da IA dos NPCs
continua pendente: o usuário informou que Kenshi não está instalado neste computador, e o código
não tem um hook de decisão/tick da IA verificado.

## Situação das três questões

| Questão | Alteração de código | Limite |
| --- | --- | --- |
| Barras de dano | Polling de vida dos sete membros e aplicação na thread do jogo | Interface e offsets precisam de teste no Kenshi |
| ReconcileLocal | Histórico de amostras, confirmação de sequência e correções graduais | Reconciliação de estado, sem replay determinístico de inputs |
| IA dos NPCs do mundo | Autoridade do host, publicação e associação de estados de NPCs carregados | Experimental; decisões locais não são suprimidas |

O protocolo passa a **3**. Cliente e servidor devem ser recompilados juntos. Os binários antigos
de dist continuam anteriores a estas mudanças.

## Vida sem depender de ApplyDamage

Antes, C2S_LimbHealth era enviado somente depois dos eventos de morte/KO. GetHealth também ignorava
falhas de leitura, e o receptor tentava chamar ApplyDamage com uma assinatura reconstruída.
Os estados de vida recebidos eram escritos pela thread de rede.

Depois:

- CharacterAccessor::ReadLimbHealth verifica a cadeia e retorna falha explícita.
- WriteLimbHealth valida os sete valores, preserva stun intercalado e verifica a região de memória.
- combat_hooks::PollHealth roda após a fila de comandos no tick do jogo.
- Amostras são lidas a cada 100 ms; mudanças acima de 0,1 são transmitidas, com refresh a cada 2 s.
- A publicação pertence ao dono do personagem ou ao host para NPCs de owner zero.
- O servidor valida origem, geração e valores finitos, mantendo health e limbHealth iguais.
- O receptor enfileira o snapshot e resolve o objeto novamente quando o comando é executado.
- É verificado também o epoch da sessão, evitando comandos antigos após troca de save/reconexão.
- A aplicação de CombatHit usa o estado de vida resultante; não chama ApplyDamage novamente.

~~~cpp
// Antes: leitura podia esconder falha; envio apenas após morte/KO.
for (int i = 0; i < 7; i++) {
    outHealth[i] = accessor.GetHealth(static_cast<BodyPart>(i));
}

// Depois: uma falha não vira um valor válido de vida.
float health[KMP_LIMB_COUNT];
if (!accessor.ReadLimbHealth(health)) {
    continue;
}
~~~

Nenhum controlador de IA é zerado e o hook ApplyDamage continua desativado. A alteração permite
sincronizar vida observada, incluindo cura e danos intermediários. Ela não cria um simulador
completo de combate de Kenshi no servidor.

## Reconciliação do personagem local

Antes, ReconcileLocal somente incrementava um contador e descartava a posição.
O servidor excluía as entidades do próprio destinatário, impedindo confirmação de estado local.

Depois, CharacterPosition inclui inputSequence; o registro de posição ocupa 32 bytes.
Os emissores locais registram a posição antes de enviá-la. O servidor memoriza e devolve a sequência
processada, rejeitando sequências antigas. Personagens locais continuam enviando amostras parados,
para receber confirmações novas.

~~~cpp
// Antes
case SnapshotDecision::ReconcileLocal:
    // TODO: Implement prediction reconciliation
    reconciledLocal++;
    break;

// Depois
case SnapshotDecision::ReconcileLocal:
    core.GetReconciliation().ReconcileSnapshot(pos, position);
    reconciledLocal++;
    break;
~~~

A correção é a diferença entre a posição do servidor e a amostra que ele confirmou.
Ela é aplicada à posição atual, preservando o movimento ocorrido depois da amostra.

Exemplo: o cliente enviou X=10, depois andou até X=13, e o servidor confirmou a primeira amostra
corrigida para X=9. A correção é -1; o destino local fica X=12, preservando os três metros posteriores.

O histórico tem limite de 128 amostras por entidade. ACKs desconhecidos/antigos, gerações erradas
e valores inválidos são rejeitados. Erros pequenos ficam numa deadzone; erros médios são suavizados;
deslocamentos grandes são aplicados uma vez. Uma escrita de posição que falha devolve a correção
para tentativa posterior. Destruição de entidades e troca de sessão limpam o histórico.

Esta etapa não reproduz pathfinding, colisões ou todos os comandos do motor no servidor.
Portanto, descreve-se como reconciliação de estado, não como rollback/replay determinístico.

## NPCs do mundo: base experimental

A política pura ai_authority.h estabelece um publicador:

~~~cpp
CanSimulateActor(type, ownerId, senderId, hostPlayerId);
// Personagem de jogador: ownerId == senderId.
// NPC do mundo com ownerId == 0: senderId == hostPlayerId e hostPlayerId != 0.
~~~

O servidor aceita NPCs de mundo somente do host. Clientes que entram depois recebem a identidade
atual do host. Personagens da facção local são classificados como PlayerCharacter, separando-os
dos NPCs de mundo.

WorldNpcSync, executado na thread do jogo, pode descobrir NPCs carregados, publicar registros e
associar réplicas a objetos existentes. A associação exige template, facção e um único candidato
num raio de três metros. A política acompanha a troca de host.

**Isso não sincroniza o cérebro da IA.** AICreate e AIPackages criam/carregam estruturas, e precisam
continuar chamando os originais. Não foi identificado um hook seguro de decisão para impedir que
cada cliente execute suas próprias escolhas.

A opção syncWorldNpcs é false por padrão. Ativá-la é um experimento de replicação de estado, com
conflitos de IA ainda possíveis. A associação por template/facção/posição também não substitui um
ID permanente de instância de save. Não se simula o mundo fora das regiões carregadas pelo host.

O próximo passo depende do executável do Kenshi: identificar e verificar o ponto de decisão,
confirmar sua assinatura e estabelecer a supressão das decisões nos clientes observadores,
preservando controladores, animações e estruturas de pathfinding.

## Testes e execução

Foram acrescentados testes C++ do código real para:

- Leitura/escrita de vida, preservação de stun, valores negativos legítimos, NaN e cadeias inválidas.
- Proteção de páginas e regiões parcialmente inacessíveis.
- Histórico/ACK de posições, movimento posterior, wrap de sequência, descarte e retry.
- Limites de memória, deadzone, smoothing, snap e acesso concorrente.
- Autoridade do host para NPCs e rejeição de controle de personagens de outro jogador.
- O formato de posição de 32 bytes, incluindo a sequência.

A máquina local continua sem Visual Studio/CMake/Kenshi. Foi acrescentado um workflow Windows que
configura e compila Release x64, roda UnitTest e roda IntegrationTest. O checkout está fixado no
commit oficial de actions/checkout v7.0.1. O resultado real do workflow deve ser consultado em Actions;
a existência do workflow não significa que os testes passaram.

Ainda são necessários testes com dois clientes reais: dano, cura, KO/morte, movimento, atraso,
reconexão e troca de save. A IA completa de NPCs não deve ser anunciada como resolvida nesta etapa.

