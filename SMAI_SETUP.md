# SMAI en TheSickMenu

## Comando

\`/ai <pregunta>\`

TheSickMenu envía la pregunta al backend SMAI mediante HTTPS. La petición es asíncrona: el juego no se bloquea mientras Groq genera la respuesta.

El cliente solo publica la respuesta si:
- El backend devuelve HTTP 200.
- El texto tiene el prefijo y sufijo requeridos por SMAI.
- El mensaje completo no supera 100 caracteres.
- No contiene emojis ni saltos de línea.

Si SMAI bloquea una respuesta (HTTP 422), el mod muestra un aviso local y no publica nada en el chat. Los errores de red o autenticación también se muestran localmente.

## Configurar el token local

El mod lee el secreto desde la variable de entorno de Windows:

\`SMAI_BOT_TOKEN\`

Su valor debe ser exactamente el mismo que la variable \`BOT_API_TOKEN\` configurada en Render.

1. Abre **Editar las variables de entorno de tu cuenta** desde el menú Inicio.
2. En **Variables de usuario**, crea \`SMAI_BOT_TOKEN\`.
3. Pega el valor de \`BOT_API_TOKEN\` de Render como valor de la variable.
4. Guarda los cambios, cierra completamente Steam/Among Us y vuelve a abrirlos para que hereden la variable.

No pongas el token en el código fuente, en GitHub, en capturas ni en mensajes. No compartas el valor conmigo.

## Memoria y privacidad

- Cada jugador se identifica mediante un HMAC-SHA256 de su Friend Code, usando el token como clave. El Friend Code original no se envía al backend.
- El backend recibe los campos de perfil disponibles: nivel, dispositivo y juego.
- El mod mantiene un historial breve en memoria RAM (hasta 8 mensajes recientes por jugador) y lo envía como contexto de la pregunta.
- La memoria de perfil se guarda en el JSON del backend. En el plan gratuito de Render ese archivo puede perderse al reiniciar o desplegar el servicio.
- No se guarda el historial completo de chat en el JSON.

**Aviso de seguridad:** cualquier secreto compartido con un cliente que se distribuye a otras personas puede terminar extraído. Mantén el mod privado mientras use un token compartido; antes de distribuirlo ampliamente conviene implementar autenticación individual o un mecanismo de acceso de corta duración.

## Estado

La integración está en la rama \`feature/thesickmenu-v2-chat-commands\`. No se ha fusionado en \`main\`.
