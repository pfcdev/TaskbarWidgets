const count = document.querySelector("#process-count");

window.taskbarWidget.on("snapshot", (snapshot) => {
  count.textContent = snapshot?.data?.processCount ?? "--";
});

document.querySelector("#task-manager").addEventListener("click", () => {
  window.taskbarWidget.invoke("openTaskManager");
});

window.taskbarWidget.ready();
