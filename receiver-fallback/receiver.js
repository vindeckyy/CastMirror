const context = cast.framework.CastReceiverContext.getInstance();
const playerManager = context.getPlayerManager();

playerManager.setMessageInterceptor(
  cast.framework.messages.MessageType.LOAD,
  loadRequestData => {
    document.getElementById('status-text').innerText = 'Streaming Desktop...';
    return loadRequestData;
  }
);

context.start();
